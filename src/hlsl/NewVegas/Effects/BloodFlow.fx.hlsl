// UNOFFICIAL, optional (Shaders.BloodFlow, off by default): blood that runs like a liquid, dries, and pools under bodies.
// BloodFlow.cpp sends the game's blood splats and the bodies' pools as items. Two passes:
//  - BloodFlowProbe (ProbePS): for each item, into a small path texture, where its streams of blood go. Each stream
//    starts on the splat's lower part and is walked down the surface the depth buffer shows, step by step, the way
//    gravity pulls along that surface (straight down a wall, slantwise across a slope, round a rock). It stops where the
//    surface turns flat (the blood gathers there: on a ledge, at the foot of a wall) or comes to an edge with nothing
//    under it (the blood hangs there and falls in drops; the drops' landing place is found the same way). A pool finds
//    the slope of the ground round it, and trickles run off its downhill side. Once a path has been seen whole for a
//    few frames it is kept, so it does not move when the camera does.
//  - BloodFlow (BloodPS): a box round each item; every pixel in it finds its surface point from the depth buffer and
//    paints what reaches it: the streams (thin trails with a bead at the front, running over the first seconds and
//    slowing), the puddles they end in, the drop hanging at an edge, drops falling (drawn in the air, in front of what
//    is behind them) and the stain where they land; the pool, stretched downhill on a slope. Blended into the image.
// The blood's colour is the light at the pixel (worked out from the image) times the blood's own colour: a thin film
// is a dark red stain the surface shows through, a thick layer is the blood's own dark red. Fresh blood shines a little
// (the sun where it is not shaded, the lamps); dried blood is darker, browner and matte. Nothing glows.
//
// UNOFFICIAL 2026-10-13 rewrite of the flow and the look (user's brief; WetWorld's scrolling rain streaks and puddles in
// hollows, by Boomstick and Mac, as the reference):
//  - Direction: each step runs along cross(cross(N, down), N), the steepest way down the surface (N x down alone runs
//    level, along the slope).
//  - Round obstacles, into valleys: the depth buffer is read as a height map round each step (Relief: how far the surface
//    there stands out of, or sinks below, the plane the blood is on). The blood leans toward the lower side, so it keeps to
//    grooves and gutters; something raised straight ahead turns it aside, to the lower side, or (no lower side) to the
//    stream's own side, so a splat's streams split round a rock or a pipe and meet below it.
//  - Motion: inside each stream the rain-streak texture is scrolled down the path (a flow map in two phases, as WetWorld's
//    lanes), each part at its own speed and drifting sideways by a smooth noise (the 3D noise texture): the blood creeps
//    and bunches unevenly instead of sliding as one sheet. Its front is lumpy and swells into a bead.
//  - Amount: every stream, puddle and pool is a thickness field (thickest in the middle, ragged by the same noise); what
//    shows is where it is thicker than a threshold (BloodAmount), so less blood breaks up into beads and clumps rather
//    than shrinking evenly. Puddles and pools reach further into the hollows round them and stay off bumps.
//  - Look: deep red; light passes through the layer (red the most) and back, so thin films and edges are a lighter,
//    saturated red and thick blood is dark (a sub-surface look); low roughness GGX highlights from the sun and lamps, a
//    sky reflection at low angles outdoors, and a red glow through thin blood when the sun is behind it.
// Settings: FlowSpeed, Viscosity (how much the noise distorts the flow: higher = slower to even out, more clumps), BloodAmount.

float4 TESR_ReciprocalResolution;
float4 TESR_SunDirection;
float4 TESR_SunColor;
float4 TESR_SunAmbient;
float4 TESR_LightPosition[12];
float4 TESR_ShadowLightPosition[12];
float4 TESR_LightColor[24];
float4 TESR_BloodFlowFresh;	// rgb: fresh blood colour (linear); w: gloss
float4 TESR_BloodFlowDry;	// rgb: dried blood colour (linear); w: splat dry time (s)
float4 TESR_BloodFlowInfo;	// x: outdoors; y: debug view; z: pool dry time (s); w: drip speed
float4 TESR_BloodFlowPool;	// x: pool spread time (s); y: longest drip (game units); z: longest fall (game units); w: gravity (units/s^2)
float4 TESR_BloodFlowProbe;	// x: 1 / path texture rows; y: finest detail (units per pixel) a path is kept at; z: rows;
							// w: 1 when the splatter atlas (TESR_BloodSplatter) is there as a loose file, else 0
float4 TESR_BloodFlowFlow;	// x: flow speed; y: viscosity; z: blood amount (0-1); w: clock (seconds, wraps every 1023)
float4 TESR_HorizonColor;
// UNOFFICIAL 2026-10-13 (user: match my regular blood's colour, resize the trails):
float4 TESR_BloodFlowMatch;		// x: the splatter texture's grey (sRGB); y: brightness; z: trail width; w: 1 match the game's colour
float4 TESR_BloodFlowDryRatio;
float4 TESR_BloodFlowBeads;		// UNOFFICIAL 2026-10-13: x wet beads' strength (0 off), y a drops tile's size in game units	// rgb: dried over fresh colour (linear, from the settings)
float4 TESR_BloodFlowShape;		// UNOFFICIAL 2026-10-13: xyz where NVR's camera-centred space puts the eye (the inverse view's translation);
							// w: 1 the vertex shader reads the bounds texture (BloodFlowBoundsVS bound, the card can)
// UNOFFICIAL 2026-10-16 (GroundFit, user: blood never clipping with the ground): x 1 the cuts that keep blood on its own surface
// fade out softly and follow the surface's slope (0: the hard cuts as before); y the fade's least width in pixels (as Terrain
// blending's MinPixels); w 1 (test only) pools act as if the probe never found the ground round them.
float4 TESR_BloodFlowGround;
// UNOFFICIAL 2026-10-27 (user: pooled and flowing blood "looks kinda grey, almost reflective"): the wet look (BloodFlow.cpp
// BloodFlowLook): x the sheen (sky or room mirrored at low angles) x what it was; y the most of it Fresnel may mirror; z how much
// of the blood's own hue the sheen takes (0: white); w the darkest a matched colour may be (linear, its strongest channel).
// OldLook: (1, 1, 0, 0), the look before exactly. Same in BloodFluidDraw.frag (look3).
float4 TESR_BloodFlowLook;
float4x4 TESR_ViewProjectionTransform;

// (Declared in register order: the samplers bind by the order they are declared in.)
sampler2D TESR_DepthBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_SourceBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_NormalsBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_PointShadowBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_DepthBufferViewModel : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_BloodFlowPaths : register(s5) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_BloodFlowBounds : register(s6) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-13 (later): now NVR's own blood runs (Textures\Effects\BloodRuns.dds, made for the user by Fable from the
// brief in NVR-Lunacy\docs\blood-texture-brief: 8 drip runs, r height, gb normal xy, a age along the run), read as .gbar so the
// code below keeps the rain streaks' layout (xy normal, a cover). Before: WetWorld's rain streaks (Boomstick and Mac): hand-drawn rivulets, thin at the top and ending in a bead
// at the bottom; rgb its normal, a its cover. Scrolled down each stream as its flowing surface detail (PaintStream).
sampler2D TESR_BloodStreaks : register(s7) < string ResourceName = "Effects\BloodRuns.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-13: the distortion noise map for the flow (Textures\Effects\NoiseVolume.dds, tools/cloud_noise.cpp:
// smooth gradient noise, r one octave 8 cells across, a another independent one). Last: it takes the next stage.
sampler3D TESR_NoiseVolume : register(s8) < string ResourceName = "Effects\NoiseVolume.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; ADDRESSW = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-13 (user: use my own blood textures rather than a big blot of red): the game's blood splatter decals,
// the texture its own splats are drawn with (Textures\decals\fxbloodsplatterdecals01.dds; the user's is a 2048 replacer):
// four splatters, 2 x 2, their shape in alpha (a dense middle, spray and droplets round it; the fourth has drip runs).
// Pools and puddles take their shape from it. NVR reads only loose files: without one, TESR_BloodFlowProbe.w is 0 and
// they keep the drawn shape.
sampler2D TESR_BloodSplatter : register(s9) < string ResourceName = "decals\fxbloodsplatterdecals01.dds"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-13: small splatters in the style of the user's blood decals (Textures\Effects\BloodSpatterSmall.dds, Fable,
// same brief): a 4 x 4 atlas, a its shape. Where falling drops land. Last: it takes the next stage.
sampler2D TESR_BloodSpatterSmall : register(s10) < string ResourceName = "Effects\BloodSpatterSmall.dds"; > = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-13: domed blood drops (Textures\Effects\BloodDrops.dds, Fable, same brief): r height, gb normal xy, a a random
// value per drop. The wet beads over the game's splats (BeadsPS). Last: it takes the next stage.
sampler2D TESR_BloodDrops : register(s11) < string ResourceName = "Effects\BloodDrops.dds"; > = sampler_state { ADDRESSU = WRAP; ADDRESSV = WRAP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
// UNOFFICIAL 2026-10-22 (SplatMask, perf): the splat shapes' mask atlas (BloodFlowBoxes::SplatMask): each splat's shape field T
// worked out once into its own cell (BloodSplatMaskBake), read by the splat pass (BloodSplatMask). R16F. Last: it takes the next stage.
sampler2D TESR_BloodSplatMask : register(s12) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
float4 TESR_BloodFlowMask;		// x: 1 the mask is there; y: E, z: D (a cell's square in splat radii: -E to E across, -E to E + D down); w: cells per atlas row
float4 TESR_BloodFlowMaskSize;	// xy: 1 / the atlas's size in texels; z: a cell's size in texels

#include "Includes/Helpers.hlsl"
#include "Includes/Depth.hlsl"
#include "Includes/Normals.hlsl"

static const float AssumedAlbedo = 0.3;	// a typical surface's brightest channel: the light at a pixel is about that over this
static const float Streams = 6.0;		// rows per item in the path texture (BloodFlowBoxes.h)
static const float PathColumns = 16.0;	// texels per row: 0 header, 1 end, 2-15 points along the path
static const float LockFrames = 3.0;	// a path seen whole this many frames running is kept
static const float WallFlatZ = 0.94;	// blood running down stops on anything flatter than about 20 degrees
static const float GroundFlatZ = 0.985;	// trickles off a pool run on down to about 10 degrees
// UNOFFICIAL 2026-10-13 (user: streaks did not run down terrain): a splat on the ground (flatter than WallFlatZ) runs downhill
// until the ground is about level, not until 20 degrees as on a wall: the Mojave's slopes are mostly gentler than that, so
// every ground splat stayed a round stain.
static const float SplatGroundFlatZ = 0.996;	// about 5 degrees
static const float UnderZ = -0.3;		// a surface facing down more than this: the blood drips off it
// UNOFFICIAL 2026-10-13 rewrite:
static const float ValleyPull = 2.5;	// how hard a difference in height across a stream steers it to the lower side
static const float Meander = 0.35;		// its own slow sideways wander
static const float StreakAcross = 0.022;	// rain-streak texture across a stream's whole width (a few of its rivulets)
static const float StreakAlong = 0.025;	// ... and along it, per game unit (40 units a tile)
static const float FlowPeriod = 3.0;	// seconds per flow-map phase (two phases half a period apart, crossfaded)
static const float CreepUnits = 2.5;	// how far the texture creeps down a stream per second at flow speed 1

struct VSIN {
	float4 position : POSITION;	// clip space (BloodFlow.cpp projects the box, or places the probe quad)
	float4 a : TEXCOORD0;		// xyz: the item's centre, eye-relative; w: slot + 1000 if a pool + 2000 if its rows are new
	float4 b : TEXCOORD1;		// xyz: a guess at the surface's normal; w: radius (splat) or largest radius (pool)
	float4 c : TEXCOORD2;		// x: age (s); y: fade; z: seed 0-4095 + 4096 x the decal's layout code (whole numbers); w: longest drip (0: none)
	float4 d : TEXCOORD3;		// UNOFFICIAL 2026-10-13: xyz: the decal texture's u axis over its width (0: not known); w: its colour,
							// 0-255 per channel packed r*65536 + g*256 + b (0: none)
};

struct VSOUT {
	float4 position : POSITION;
	float4 a : TEXCOORD0;
	float4 b : TEXCOORD1;
	float4 c : TEXCOORD2;
	float4 d : TEXCOORD3;
};

struct PSIN {
	float4 a : TEXCOORD0;
	float4 b : TEXCOORD1;
	float4 c : TEXCOORD2;
	float4 d : TEXCOORD3;
	float2 vpos : VPOS;
};

VSOUT BoxVS(VSIN IN) {
	VSOUT OUT;
	OUT.position = IN.position;
	OUT.a = IN.a;
	OUT.b = IN.b;
	OUT.c = IN.c;
	OUT.d = IN.d;
	return OUT;
}

// The item's seed, 0-1 (VSIN c.z: the low 12 bits).
float ItemSeed(float cz) { return fmod(floor(cz + 0.5), 4096.0) / 4096.0; }

float hash1(float n) {
	return frac(sin(n) * 43758.5453);
}

float hash2(float2 p) {
	return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float valueNoise(float2 p) {
	float2 i = floor(p);
	float2 f = frac(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = hash2(i);
	float b = hash2(i + float2(1, 0));
	float c = hash2(i + float2(0, 1));
	float d = hash2(i + float2(1, 1));
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float fbm(float2 p) {
	return 0.6 * valueNoise(p) + 0.3 * valueNoise(p * 2.13 + 5.2) + 0.1 * valueNoise(p * 4.71 + 1.7);
}

// The distortion noise map: smooth noise 0-1, one cell per unit of p (two independent ones: r and a).
float2 FlowNoise(float3 p) { return tex3Dlod(TESR_NoiseVolume, float4(p / 8.0, 0.0)).ra; }

// One of the splatter atlas's first three splatters (picked and turned by seed): its cover at local (-0.5 to 0.5 across
// it), read at the mip a pixel of `footprint` splatter widths needs. 0 outside it.
float Splatter(float2 local, float seed, float footprint) {
	float pick = floor(frac(seed * 13.7) * 3.0);
	float angle = seed * 37.0;
	float2 r = float2(local.x * cos(angle) - local.y * sin(angle), local.x * sin(angle) + local.y * cos(angle));
	float2 uv = (float2(fmod(pick, 2.0), floor(pick * 0.5)) + 0.5 + clamp(r, -0.48, 0.48)) * 0.5;
	// UNOFFICIAL 2026-10-20 (user: 'still saw a circular pool'): a small pool read the atlas so far down its mips that the
	// splatter blurred into a round blob, and above the threshold that is a disc. At most mip 2 (the edge is smoothed by Shows).
	return tex2Dlod(TESR_BloodSplatter, float4(uv, 0.0, min(log2(max(footprint * 1024.0, 1.0)), 2.0))).a * step(abs(r.x), 0.48) * step(abs(r.y), 0.48);
}

// UNOFFICIAL 2026-10-13 (user: streams should come out of the splatter's blood, not from the edge of its texture): the cover
// (alpha) of the game's own splatter texture at a point `rel` (item-relative, on the splat's surface), laid out the way the
// game laid that decal (BloodFlow.cpp, DecalLayout: VSIN d.xyz is its u axis over its width, c.z / 4096 its code), read at
// `mip`. -1 when the layout or the texture is not known; 0 outside the decal.
float DecalCover(float3 rel, float4 b, float4 c, float4 d, float mip) {
	if (TESR_BloodFlowProbe.w < 0.5 || dot(d.xyz, d.xyz) < 1e-12) return -1.0;
	float code = floor(floor(c.z + 0.5) / 4096.0);
	float quarter = fmod(code, 5.0);
	float ratio = exp2(floor(code / 20.0) / 31.5 - 1.0);	// width over height
	float3 v = cross(normalize(b.xyz), d.xyz) * ratio * (fmod(floor(code / 10.0), 2.0) > 0.5 ? -1.0 : 1.0);
	float2 uv = float2(dot(rel, d.xyz), dot(rel, v)) + 0.5;
	if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
	uv = quarter < 3.5 ? uv * 0.5 + float2(fmod(quarter, 2.0), floor(quarter * 0.5)) * 0.5 : uv;
	if (fmod(floor(code / 5.0), 2.0) > 0.5) uv.x = 1.0 - uv.x;
	return tex2Dlod(TESR_BloodSplatter, float4(uv, 0.0, mip)).a;
}

// One of the 16 small splatters (picked and turned by seed): its cover at local (-0.5 to 0.5 across it), read at the mip a pixel
// of `footprint` splatter widths needs. 0 outside it.
float SmallSplatter(float2 local, float seed, float footprint) {
	float pick = floor(frac(seed * 29.3) * 16.0);
	float angle = seed * 53.0;
	float2 r = float2(local.x * cos(angle) - local.y * sin(angle), local.x * sin(angle) + local.y * cos(angle));
	float2 uv = (float2(fmod(pick, 4.0), floor(pick * 0.25)) + 0.5 + clamp(r, -0.48, 0.48)) * 0.25;
	return tex2Dlod(TESR_BloodSpatterSmall, float4(uv, 0.0, log2(max(footprint * 256.0, 1.0)))).a * step(abs(r.x), 0.48) * step(abs(r.y), 0.48);
}

// UNOFFICIAL 2026-10-13 (user screenshot: a perfectly round pool): the solid middle kept under a splatter shape, 0.5 at the
// centre falling to 0 at `r` from it, its edge pushed in and out by smooth noise round the way out (continuous all round), so
// where the splatter is thin the pool is a blob, never a coin.
float RaggedCore(float2 d, float r, float seed) {
	float2 way = d / max(length(d), 1e-4);
	float edge = r * (0.65 + 0.7 * valueNoise(way * 1.7 + seed * 23.0));
	return saturate(1.0 - length(d) / max(edge, 1e-3)) * 0.5;
}

// How much of a thickness T shows: where it is above the threshold BloodAmount sets (1 - amount), the edge smoothed over
// UNOFFICIAL 2026-10-13: the bounds texture again, read by the vertex shader (CutBox). Not a TESR_ name, so the effect's own
// samplers keep their order; BloodFlow.cpp binds it to the first vertex sampler itself.
sampler2D BloodFlowBoundsVS : register(vs_3_0, s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

// dT, the change in T across one pixel.
float Shows(float T, float dT) { return saturate((T - 1.0 + TESR_BloodFlowFlow.z) / max(dT, 0.02) + 0.5); }

// The item's slot code (VSIN a.w): slot, pool, fresh rows.
void DecodeSlot(float w, out float slot, out float pool, out float fresh) {
	float code = floor(w + 0.5);
	fresh = code >= 2000.0 ? 1.0 : 0.0;
	code -= fresh * 2000.0;
	pool = code >= 1000.0 ? 1.0 : 0.0;
	slot = code - pool * 1000.0;
}

float4 PathTexel(float column, float row) {
	return tex2Dlod(TESR_BloodFlowPaths, float4((column + 0.5) / PathColumns, (row + 0.5) * TESR_BloodFlowProbe.x, 0.0, 0.0));
}

// ---------------------------------------------------------------------------------------------------------------------
// Streams: where each starts, how far and how fast it runs. The same in both passes (the probe walks the path, the
// paint pass shows as much of it as has run by now).

float4 TESR_BloodFlowSplat;	// x: strength (0 off), y: how far the arms and spray reach (x the splat's radius), z: spray and mist amount, w: drip length

// Distance from p to the segment a-b, and how far along it (0-1).
float SegmentDistance(float2 p, float2 a, float2 b, out float t) {
	float2 ab = b - a;
	t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-6));
	return length(p - a - ab * t);
}

// The core's radius in the direction `a` (lumpy, longer down a wall as it sags).
float SplatCoreRadius(float a, float seed, float grow) {
	float c = (0.45 + 0.18 * hash1(seed * 7.13)) * (0.6 + 0.4 * grow);
	return c * (1.0 + 0.26 * (valueNoise(float2(a * 1.4 + seed * 11.0, seed * 3.0)) - 0.5) * 2.0 + 0.07 * sin(a * 6.0 + seed * 19.0) + 0.05 * sin(a * 13.0 + seed * 7.0));
}

// UNOFFICIAL 2026-10-14: arm i of a splat (shared by its shape, SplatThickness, and the probe, which starts a real stream at the
// tip of each of the first arms: user: the spray and arms flow like liquid over the real surfaces). In splat radii.
float SplatArms(float seed) { return 5.0 + floor(hash1(seed * 9.17) * 7.0); }

// UNOFFICIAL 2026-10-20 (user: slow and 'comically violent'; log: 13 splats = 33 ms): the paint-splat shape is drawn at most this
// big (game units of radius); the game's decals reach 60, and with spray round them a few close ones filled the screen.
float ShapeR(float r) { return min(r, 24.0); }

void SplatArm(float i, float seed, float grow, out float2 dir, out float rootR, out float len, out float width) {
	float slant = 0.3 + 0.7 * hash1(seed * 3.71);
	float lean = hash1(seed * 5.93) * 6.2832;
	float2 leanDir = float2(cos(lean), sin(lean));
	float h1 = hash1(seed * 13.7 + i * 7.31), h2 = hash1(seed * 29.3 + i * 3.17), h3 = hash1(seed * 41.9 + i * 5.53), h4 = hash1(seed * 57.1 + i * 2.29);
	dir = float2(cos(h1 * 6.2832), sin(h1 * 6.2832));
	float2 bent = lerp(dir, leanDir, slant * 0.6 * h4);
	dir = dot(bent, bent) > 1e-4 ? normalize(bent) : dir;
	float along = dot(dir, leanDir) * slant;
	rootR = SplatCoreRadius(atan2(dir.y, dir.x), seed, grow) * 0.7;
	len = (rootR + (0.3 + 0.9 * h2 * h2) * TESR_BloodFlowSplat.y * (1.0 + along * 0.7)) * grow;
	width = 0.05 + 0.1 * h3;
}

struct Stream {
	float active;
	float2 start;		// splats: across and down the splat's surface from its centre
	float lengthMax;	// the most it runs, along the surface
	float tau;			// how fast (most of the way in about twice this, in seconds)
	float delay;		// seconds before it starts
	float width;		// half its width
	float dropVolume;	// how much run (in length of stream) makes one drop at an edge
};

Stream SplatStream(float seed, float k, float R, float dripMax, float speed) {
	Stream s;
	float s1 = hash1(seed * 91.7 + k * 13.1);
	float s2 = hash1(seed * 47.3 + k * 7.7);
	float s3 = hash1(seed * 23.9 + k * 3.3);
	s.active = (s3 >= 0.3 && dripMax > 0.0) ? 1.0 : 0.0;	// not every part of a splat runs
	float x0 = (s1 * 2.0 - 1.0) * 0.6 * R;
	s.start = float2(x0, R * (0.35 + 0.35 * s2) * sqrt(saturate(1.0 - sqr(x0 / R))));	// from near the splat's lower edge
	s.lengthMax = dripMax * (0.3 + 0.7 * s3) * saturate(R / 6.0 + 0.3);				// bigger splats run further
	s.tau = (1.5 + 3.5 * s2) / speed;
	s.delay = (0.2 + 1.2 * s1) / speed;
	s.width = clamp(R * (0.05 + 0.05 * s1), 0.35, 2.0) * TESR_BloodFlowMatch.z;
	s.dropVolume = (2.5 + 2.5 * s2) * s.width;
	return s;
}

// A trickle running off a pool on a slope (k 0 or 1). slope: the ground's rise over run.
Stream PoolStream(float seed, float k, float R, float slope, float spreadTime) {
	Stream s;
	float s1 = hash1(seed * 71.3 + k * 17.9);
	float s2 = hash1(seed * 33.1 + k * 5.3);
	float s3 = hash1(seed * 12.7 + k * 9.1);
	s.active = 1.0;
	s.start = float2(0.0, 0.0);
	s.lengthMax = R * (0.8 + 2.2 * saturate(slope * 2.0)) * (0.6 + 0.4 * s3);
	s.tau = spreadTime * (0.8 + 0.6 * s2);
	s.delay = spreadTime * (0.25 + 0.2 * s1);
	s.width = clamp(R * 0.06 * (0.8 + 0.4 * s1), 0.8, 2.5) * TESR_BloodFlowMatch.z;
	s.dropVolume = (2.5 + 2.5 * s2) * s.width;
	return s;
}

// How far a stream has run by `age` (it slows as it goes).
float StreamRun(Stream s, float lengthMax, float age) {
	return lengthMax * (1.0 - exp(-max(age - s.delay, 0.0) / s.tau));
}

// When a stream has run `run` (inverse of StreamRun).
float StreamTime(Stream s, float lengthMax, float run) {
	return s.delay - s.tau * log(max(1.0 - run / lengthMax, 1e-4));
}

// A pool's middle moves downhill on a slope and it stretches that way (slope: rise over run).
float PoolShift(float slope) { return 0.35 * saturate(slope * 2.0); }
float PoolStretch(float slope) { return 1.0 + 0.9 * saturate(slope * 2.0); }

// ---------------------------------------------------------------------------------------------------------------------
// The probe: what the depth buffer shows.

float3 CameraRight() { return float3(TESR_ViewTransform[0][0], TESR_ViewTransform[1][0], TESR_ViewTransform[2][0]); }
float3 CameraUp() { return float3(TESR_ViewTransform[0][1], TESR_ViewTransform[1][1], TESR_ViewTransform[2][1]); }
float3 CameraForward() { return float3(TESR_ViewTransform[0][2], TESR_ViewTransform[1][2], TESR_ViewTransform[2][2]); }

// An eye-relative point -> (u, v, view depth); the inverse of toWorld(uv) * depth.
float3 ProjectRel(float3 rel) {
	float z = dot(rel, CameraForward());
	float2 ndc = float2(dot(rel, CameraRight()) * TESR_ProjectionTransform[0][0], dot(rel, CameraUp()) * TESR_ProjectionTransform[1][1]) / max(z, 0.001);
	return float3(0.5 + 0.5 * ndc.x, 0.5 - 0.5 * ndc.y, z);
}

// What the depth buffer shows on the line of sight through Q (eye-relative): X, the surface there, and N its normal;
// along: how far that surface is behind Q along the line (negative: in front of it); tol: how far off still counts as
// on it (more for a coarser step, a far surface or one seen edge on). False when Q cannot be seen: off screen, behind
// the eye or behind the weapon.
bool SeeSurface(float3 Q, float stepSize, out float3 X, out float3 N, out float along, out float tol) {
	float3 s = ProjectRel(Q);
	X = Q;
	N = float3(0.0, 0.0, 1.0);
	along = 0.0;
	tol = 1.0;
	if (s.z < nearZ * 2.0 || s.x < 0.002 || s.y < 0.002 || s.x > 0.998 || s.y > 0.998) return false;
	s.xy = screenToRect(s.xy);	// UNOFFICIAL 2026-10-27 RenderScale v2: the screen position tested, the texture coordinate read
	float4 uv = float4(s.xy, 0.0, 0.0);
	float viewmodel = tex2Dlod(TESR_DepthBufferViewModel, uv).x;
	if ((TESR_DepthConstants.z == 0 && viewmodel < 0.9) || (TESR_DepthConstants.z > 0 && viewmodel > 0.01)) return false;
	float depth = readDepthLod(s.xy);
	float dist = length(Q);
	if (depth >= farZ * 0.98) { along = 1e6; return true; }		// sky: nothing behind it
	X = toWorld(s.xy) * depth;
	N = normalize(GetWorldNormalLod(s.xy));
	along = (depth - s.z) * dist / s.z;
	float pixel = s.z * 2.0 * TESR_ReciprocalResolution.y / TESR_ProjectionTransform[1][1];
	tol = (0.75 + 0.5 * stepSize + 2.0 * pixel) / max(abs(dot(N, Q / dist)), 0.2);
	return true;
}

// The depth buffer as a height map: how far the surface seen through Q stands out of (+) or sinks below (-) the plane
// through pos with normal N, at most `limit` either way (something in front of Q reads as raised). 0 when Q is off screen;
// a hole when there is only sky behind it.
float Relief(float3 Q, float3 pos, float3 N, float limit) {
	float3 s = ProjectRel(Q);
	if (s.z < nearZ * 2.0 || s.x < 0.002 || s.y < 0.002 || s.x > 0.998 || s.y > 0.998) return 0.0;
	s.xy = screenToRect(s.xy);	// UNOFFICIAL 2026-10-27 RenderScale v2
	float depth = readDepthLod(s.xy);
	if (depth >= farZ * 0.98) return -limit;
	return clamp(dot(toWorld(s.xy) * depth - pos, N), -limit, limit);
}

// Runs blood from S (eye-relative, on the surface, normal N) down the surface for up to `target` units, the way gravity
// pulls along it, in steps of h. Returns where it got to (xyz) and how far it ran (w). end: 0 it ran the whole way,
// 1 it stopped on something flat (it gathers there), 2 it came to an edge with nothing under it (it drips off there).
// valid becomes 0 if part of the way could not be seen (the path then carries on along the last surface seen).
// UNOFFICIAL 2026-10-13: width (the stream's half width) sets how far round it the relief is read; side (-1 or 1) is the
// way it goes round something raised in its path when neither side is lower.
float4 RunDown(float3 S, float3 N, float target, float h, float flatZ, float wiggleSeed, float width, float side, inout float valid, out float end) {
	float3 pos = S;
	float arc = 0.0;
	float going = 0.0;		// going round an obstacle: the side (-1, 1), 0 when not
	end = 0.0;
	[loop] for (int i = 0; i < 48; i++) {
		if (arc >= target - 0.001) break;
		if (N.z > flatZ) { end = 1.0; break; }
		if (N.z < UnderZ) { end = 2.0; break; }
		float stepSize = min(h, target - arc);
		// Steepest way down: level (across) is N x down, down the slope is across x N.
		float3 across = normalize(cross(N, float3(0.0, 0.0, -1.0)));
		float3 d = cross(across, N);
		// The relief either side a little ahead, and straight ahead: lean to the lower side (valleys); turn aside from
		// something raised ahead, to its lower side, or the stream's own, kept until the way ahead is clear.
		float reach = width * 1.5 + 0.75;
		float limit = reach * 2.0 + stepSize;
		float hLeft = Relief(pos + (d * 0.5 + across) * reach, pos, N, limit);
		float hRight = Relief(pos + (d * 0.5 - across) * reach, pos, N, limit);
		// (Ahead, only something steeper than a resting place counts: a floor at the foot of a wall or a ledge also stands
		// out of the wall's plane, but there the blood stops, below.)
		float hAhead = 0.0;
		float3 Xa, Na;
		float alongA, tolA;
		if (SeeSurface(pos + d * (stepSize + width), stepSize, Xa, Na, alongA, tolA) && Na.z <= flatZ) hAhead = clamp(dot(Xa - pos, N), -limit, limit);
		float lean = clamp((hRight - hLeft) / reach * ValleyPull, -1.2, 1.2);
		float raised = saturate((hAhead - 0.35 * stepSize - 0.3) / (0.5 * stepSize + 0.5));
		if (raised > 0.3 && going == 0.0) going = abs(hRight - hLeft) > 0.2 ? sign(hRight - hLeft) : side;
		if (raised < 0.05) going = 0.0;
		float wander = (valueNoise(float2(arc * 0.07 + wiggleSeed, wiggleSeed * 3.1)) - 0.5) * 2.0 * Meander;
		d = normalize(d + across * (lean + going * raised * 2.5 + wander));
		float3 next = pos + d * stepSize;
		float3 X, N2;
		float along, tol;
		if (!SeeSurface(next, stepSize, X, N2, along, tol)) { valid = 0.0; pos = next; arc += stepSize; continue; }
		if (abs(along) <= tol) { pos = X; N = N2; arc += stepSize; continue; }
		if (along > 0.0) {
			// Nothing under the next step: an edge. Find the brink by halving the step twice.
			float sz = stepSize;
			[unroll] for (int b = 0; b < 2; b++) {
				sz *= 0.5;
				float3 Xm, Nm;
				float am, tm;
				if (SeeSurface(pos + d * sz, sz, Xm, Nm, am, tm) && abs(am) <= tm) { pos = Xm; arc += sz; }
			}
			// Round the edge: if there is a surface right under the brink (the face below a slope's edge, not the
			// underside of an overhang), the blood goes on down it.
			float3 Xw, Nw;
			float aw, tw;
			if (SeeSurface(pos - float3(0.0, 0.0, h), h, Xw, Nw, aw, tw) && abs(aw) <= tw && Nw.z > UnderZ && Nw.z < flatZ) {
				pos = Xw;
				N = Nw;
				arc += h;
				continue;
			}
			end = 2.0;
			break;
		}
		if (N2.z > 0.6 && X.z - next.z >= -tol && X.z - next.z <= stepSize + tol) {
			// Into something standing out under it (a ledge, the floor): the blood stops on its top.
			pos = float3(next.xy, X.z);
			arc += stepSize;
			end = 1.0;
			break;
		}
		// Hidden behind something: carry on along the same surface.
		valid = 0.0;
		pos = next;
		arc += stepSize;
	}
	return float4(pos, arc);
}

// Falls straight down from E (eye-relative) until something under it is seen. Returns how far, or -1 if nothing within
// maxFall.
float FallFrom(float3 E, float maxFall, inout float valid) {
	float stepSize = max(maxFall / 24.0, 2.0);
	[loop] for (int j = 1; j <= 24; j++) {
		float s = j * stepSize;
		if (s > maxFall + 0.01) break;
		float3 F = E - float3(0.0, 0.0, s);
		float3 X, N;
		float along, tol;
		if (!SeeSurface(F, stepSize, X, N, along, tol)) { valid = 0.0; continue; }
		if (along > tol) continue;										// still in the air
		if (N.z > 0.5 && X.z >= F.z - tol && X.z <= F.z + stepSize + tol) return max(E.z - X.z, 0.0);	// the ground
		valid = 0.0;													// something in front hides it
	}
	return -1.0;
}

// The probe pass: one texel per (column, row) of the path texture, for the item whose quad covers it.
//  Splat stream rows, and the trickle rows (0, 1) of a pool:
//   column 0: (frames seen whole, how far it runs before it stops, how far drops fall from its end (-1: no ground seen),
//              end: -1 no stream, 0 runs its length, 1 gathers on the flat, 2 drips off an edge)
//   column 1: (where it stops, item-relative; longest run)
//   columns 2-15: points along the path (item-relative xyz, distance along it), evenly spaced over the longest run
//  A pool's last row (the ground round it): column 0 (frames seen whole, points seen, 0, -2), column 1 (the ground's
//   normal, its height under the centre relative to the centre).
float4 ProbePS(PSIN IN) : COLOR0 {
	float slot, pool, fresh;
	DecodeSlot(IN.a.w, slot, pool, fresh);
	float column = floor(IN.vpos.x);
	float row = floor(IN.vpos.y);
	float k = row - slot * Streams;
	float4 previous = PathTexel(column, row);
	float4 previousHead = PathTexel(0.0, row);
	if (fresh < 0.5 && previousHead.x >= LockFrames) return previous;	// kept
	float seenBefore = fresh > 0.5 ? 0.0 : previousHead.x;

	float3 C = IN.a.xyz;
	float R = IN.b.w;
	float seed = ItemSeed(IN.c.z);
	// Too far away to see the surface's shape well: worked out every frame, never kept.
	float3 cs = ProjectRel(C);
	float pixel = cs.z * 2.0 * TESR_ReciprocalResolution.y / TESR_ProjectionTransform[1][1];
	float valid = pixel <= TESR_BloodFlowProbe.y ? 1.0 : 0.0;

	if (pool > 0.5 && k > Streams - 1.5) {
		// The ground round the pool: a ring of points just outside it, where the ground (not the body) shows. Twice: the
		// second time the points follow the slope the first found, and only ground facing about the same way counts
		// (not an arm or a leg).
		float3 groundGuess = float3(0.0, 0.0, 1.0);
		float3 sumN = 0.0;
		float3 sumX = 0.0;
		float n = 0.0;
		[loop] for (int round = 0; round < 2; round++) {
			sumN = 0.0;
			sumX = 0.0;
			n = 0.0;
			[loop] for (int i = 0; i < 12; i++) {
				float angle = i * 0.5236;
				float2 off = float2(cos(angle), sin(angle)) * R * 1.2;
				float3 Q = C + float3(off, -(groundGuess.x * off.x + groundGuess.y * off.y) / groundGuess.z);
				float3 X, N;
				float along, tol;
				if (!SeeSurface(Q, 2.0, X, N, along, tol)) continue;
				if (N.z < 0.5 || abs(X.z - Q.z) > R * 0.6 + 8.0 || length(X.xy - Q.xy) > 6.0 + R * 0.6) continue;
				if (round > 0 && dot(N, groundGuess) < 0.97) continue;
				sumN += N;
				sumX += X;
				n += 1.0;
			}
			if (n > 0.0) groundGuess = normalize(sumN);
			groundGuess.z = max(groundGuess.z, 0.5);
		}
		if (n < 5.0) valid = 0.0;
		float3 groundN = n > 0.0 ? normalize(sumN) : float3(0.0, 0.0, 1.0);
		float3 middleSeen = n > 0.0 ? sumX / n : C;
		float heightAtCentre = middleSeen.z - (groundN.x * (C.x - middleSeen.x) + groundN.y * (C.y - middleSeen.y)) / max(groundN.z, 0.3);
		if (column < 0.5) return float4(valid > 0.5 ? seenBefore + 1.0 : 0.0, n, 0.0, -2.0);
		return float4(groundN, heightAtCentre - C.z);
	}

	Stream s;
	float3 S, N;
	float flatZ;
	float none = 0.0;			// no stream: 1 for good (kept at once), 2 for now (worked out again next frame)
	if (pool > 0.5) {
		// A trickle off the pool's downhill side, once the ground round it is known (last frame's) and slopes.
		float groundRow = slot * Streams + Streams - 1.0;
		float4 groundHead = PathTexel(0.0, groundRow);
		float4 ground = PathTexel(1.0, groundRow);
		bool known = fresh < 0.5 && groundHead.y >= 5.0;
		bool groundKept = known && groundHead.x >= LockFrames;
		float slope = known ? length(ground.xy) / max(ground.z, 0.3) : 0.0;
		s = PoolStream(seed, k, R, slope, TESR_BloodFlowPool.x);
		flatZ = GroundFlatZ;
		if (k > 1.5) none = 1.0;
		else if (!known || slope < 0.12) none = groundKept ? 1.0 : 2.0;
		float2 downhill = slope > 0.0 ? normalize(ground.xy) : float2(1.0, 0.0);
		float turn = (k < 0.5 ? -0.5 : 0.5) + (hash1(seed * 19.3 + k) - 0.5) * 0.4;
		float2 way = float2(downhill.x * cos(turn) - downhill.y * sin(turn), downhill.x * sin(turn) + downhill.y * cos(turn));
		float2 offset = downhill * R * PoolShift(slope) + way * R * 0.9;
		S = C + float3(offset, ground.w - (ground.x * offset.x + ground.y * offset.y) / max(ground.z, 0.3));
		N = known ? ground.xyz : float3(0.0, 0.0, 1.0);
		if (!groundKept) valid = 0.0;
	}
	else {
		s = SplatStream(seed, k, R, IN.c.w, TESR_BloodFlowInfo.w);
		flatZ = WallFlatZ;
		// The surface at the splat's centre: its slope decides where the streams start.
		float3 X0, N0;
		float along0, tol0;
		bool seen0 = SeeSurface(C, 2.0, X0, N0, along0, tol0) && abs(along0) <= R * 0.5 + 4.0;
		if (!seen0) valid = 0.0;
		float3 plane = seen0 ? N0 : normalize(IN.b.xyz);
		if (plane.z > WallFlatZ) flatZ = SplatGroundFlatZ;	// on the ground: downhill until about level
		if (s.active < 0.5) none = 1.0;
		else if (plane.z > flatZ) none = seen0 ? 1.0 : 2.0;	// on a floor: it stays where it is
		float3 down = float3(plane.x * plane.z, plane.y * plane.z, plane.z * plane.z - 1.0);
		down = length(down) > 0.01 ? normalize(down) : float3(0.0, 0.0, -1.0);
		float3 across = normalize(cross(plane, down));
		// UNOFFICIAL 2026-10-13: with the game's splatter texture and its layout known, each stream starts where that splat's
		// blood is thickest (its blobs, read at a coarse mip, not its speckles), lower parts preferred, among 16 points picked
		// for this stream: so it runs out of the blood the game drew, not out of the edge of its texture.
		float3 base = seen0 ? X0 : C;
		// UNOFFICIAL 2026-10-14 (user: like a balloon of paint thrown at a wall, splattering, dripping, running): how much blood
		// there is where a stream starts decides whether it runs at all and how far. A small splat has only a few streams (a
		// fine spray none), a big one all six; a thin part of a splat does not drip, a thick blob runs furthest. The amount
		// reaches the paint pass through the longest run stored with the path (BloodPS, StreamState: wider, sooner).
		[branch] if (TESR_BloodFlowSplat.x > 0.0 && k < SplatArms(seed)) {
			// UNOFFICIAL 2026-10-14 (user: the spray lines and arms flow, drip, like liquid): with the splat's own shape on, stream
			// k runs from the tip of arm k (SplatArm), over the real surfaces, round things and off edges; a thick arm runs
			// further and wider, a thin one only sometimes. The shape draws no drip there (SplatPS reads this row).
			float2 dir;
			float rootR, len, width;
			SplatArm(k, seed, 1.0, dir, rootR, len, width);
			s.start = dir * (len + width * 0.5) * ShapeR(R);
			float thick = saturate((width - 0.05) / 0.1);
			if (hash1(seed * 57.1 + k * 2.29) > 0.35 + 0.6 * thick) none = 1.0;
			s.lengthMax *= lerp(0.5, 1.4, thick) * TESR_BloodFlowSplat.w;
		}
		else {
		if (hash1(seed * 7.31 + k * 3.17) > saturate(R / 10.0) + 0.2) none = 1.0;
		[branch] if (dot(IN.d.xyz, IN.d.xyz) > 1e-12 && TESR_BloodFlowProbe.w > 0.5) {
			float bestScore = -1.0;
			float bestCover = 0.0;
			[loop] for (int i = 0; i < 16; i++) {
				float2 cand = float2((hash1(seed * 53.1 + k * 11.3 + i * 1.71) * 2.0 - 1.0) * 0.8 * R,
					(hash1(seed * 29.7 + k * 5.9 + i * 2.37) * 1.1 - 0.25) * R);	// across; down from a little above the centre
				float cover = DecalCover(base - C + across * cand.x + down * cand.y, IN.b, IN.c, IN.d, 5.0);
				float score = cover + 0.3 * cand.y / R;
				if (cover > 0.35 && score > bestScore) { bestScore = score; bestCover = cover; s.start = cand; }
			}
			if (bestCover < 0.45) none = 1.0;	// nothing thick enough on this part of the splat to drip
			s.lengthMax *= lerp(0.35, 1.4, saturate((bestCover - 0.45) / 0.5));
		}
		}
		S = base + across * s.start.x + down * s.start.y;
		N = plane;
	}
	if (none > 0.5) {
		if (column < 0.5) return float4(none < 1.5 ? LockFrames : 0.0, 0.0, 0.0, -1.0);
		return float4(0.0, 0.0, 0.0, 0.0);
	}
	// Onto the surface.
	float3 X, N1;
	float along, tol;
	if (SeeSurface(S, 2.0, X, N1, along, tol) && abs(along) <= tol * 2.0) { S = X; N = N1; }
	else valid = 0.0;

	float h = max(s.lengthMax / 40.0, 0.6);
	float target = column < 1.5 ? s.lengthMax : (column - 2.0) * s.lengthMax / 13.0;
	float end;
	// Round something in the way, a stream goes the side it started on (a pool's two trickles: one each way).
	float side = s.start.x + (k - 0.5) * 0.001 < 0.0 ? -1.0 : 1.0;
	float4 reached = RunDown(S, N, target, h, flatZ, seed * 61.0 + k * 7.0, s.width, side, valid, end);
	if (column > 1.5) return float4(reached.xyz - C, reached.w);
	if (column > 0.5) return float4(reached.xyz - C, s.lengthMax);
	float fall = 0.0;
	if (end > 1.5) fall = FallFrom(reached.xyz, TESR_BloodFlowPool.z, valid);	// (not ?: : HLSL works out both sides)
	return float4(valid > 0.5 ? seenBefore + 1.0 : 0.0, reached.w, fall, end);
}

// ---------------------------------------------------------------------------------------------------------------------
// The bounds pass (BoundsPS): for each item (row = its slot, from the path texture just written), the box round all
// its blood can reach, item-relative: column 0 (lowest corner, 1), column 1 (highest corner, 1). The paint pass drops
// every pixel outside it at once (the box drawn round an item has to allow for any path; this one is the real paths).

float4 BoundsPS(PSIN IN) : COLOR0 {
	float slot, pool, fresh;
	DecodeSlot(IN.a.w, slot, pool, fresh);
	float column = floor(IN.vpos.x);
	float R = IN.b.w;
	float seed = ItemSeed(IN.c.z);
	float firstRow = slot * Streams;
	float3 lo, hi;
	float slope = 0.0;
	float count = Streams;
	if (pool > 0.5) {
		// The pool, moved and stretched downhill (see BloodPS), on the ground's plane.
		float4 groundHead = PathTexel(0.0, firstRow + Streams - 1.0);
		float4 ground = PathTexel(1.0, firstRow + Streams - 1.0);
		bool known = groundHead.y >= 5.0 && TESR_BloodFlowGround.w < 0.5;
		float3 gN = known ? ground.xyz : float3(0.0, 0.0, 1.0);
		slope = length(gN.xy) / max(gN.z, 0.3);
		float reach = R * (1.45 * PoolStretch(slope) + PoolShift(slope)) + 2.0;
		// UNOFFICIAL 2026-10-20 (user: half pools left, cut straight): BloodPS now follows the surface's own plane (the probe's is
		// tilted by grass and ragdolls), so this box must not use the probe's height either: with GroundFit it allows any
		// slope up to about 30 degrees round the pool's own centre, as when the ground is not found.
		bool fit = TESR_BloodFlowGround.x > 0.5;
		float gz = known && !fit ? ground.w : 0.0;
		// UNOFFICIAL 2026-10-16 (GroundFit): with the ground round it not found, room for a slope up to about 30 degrees (BloodPS
		// then follows the surface's own slope), and for the soft edge.
		float unsure = fit ? reach * 0.6 + 3.0 : 0.0;
		lo = float3(-reach, -reach, gz - reach * slope - 8.0 - 0.45 * reach - unsure);
		hi = float3(reach, reach, gz + reach * slope + 5.0 + 0.25 * reach + unsure);
		count = 2.0;
	}
	else {
		// The game's splat itself (it dries too).
		lo = -(R * 1.1 + 1.0);
		hi = R * 1.1 + 1.0;
	}
	[loop] for (float k = 0.0; k < count; k += 1.0) {
		float row = firstRow + k;
		float4 head = PathTexel(0.0, row);
		if (head.w < -0.5) continue;
		float4 last = PathTexel(1.0, row);
		float3 first = PathTexel(2.0, row).xyz;
		Stream s;
		if (pool > 0.5) s = PoolStream(seed, k, R, slope, TESR_BloodFlowPool.x);
		else s = SplatStream(seed, k, R, IN.c.w, TESR_BloodFlowInfo.w);
		// The path strays at most this far from the line from its start to its end (see FindPath); its puddle at most
		// this big.
		float3 chord = last.xyz - first;
		float stray = 0.5 * sqrt(max(head.y * head.y - dot(chord, chord), 0.0));
		float grow = stray + s.width * 4.0 + 3.0 + sqrt(max(last.w - head.y, 0.0) * s.width);
		lo = min(lo, min(first, last.xyz) - grow);
		hi = max(hi, max(first, last.xyz) + grow);
		if (head.w > 1.5) {
			// Drops falling from its end, and their stain.
			float3 L = last.xyz - float3(0.0, 0.0, head.z > 0.0 ? head.z : TESR_BloodFlowPool.z);
			float spread = s.width * 10.0 + 3.0;
			lo = min(lo, L - spread);
			hi = max(hi, float3(L.xy + spread, L.z + 3.0));
		}
	}
	return column < 0.5 ? float4(lo, 1.0) : float4(hi, 1.0);
}

// ---------------------------------------------------------------------------------------------------------------------
// The paint pass.

// What blood a pixel gets: the strongest thing that reaches it decides how thick it is and how it leans.
struct Paint {
	float cover;	// 0-1 how much of the pixel's surface it covers
	float thick;	// 0 a thin film (a stain the surface shows through) - 1 a thick layer (pool, drop)
	float3 bend;	// how its surface leans (rounded trails and drops), added to the surface's normal
	float air;		// 0-1 a falling drop in front of the surface
	float debug;	// 1 trail, 2 puddle, 3 edge drop, 4 landing stain
};

void Add(inout Paint p, float c, float thick, float3 bend, float debug) {
	if (c > p.cover) {
		p.thick = thick;
		p.bend = bend;
		p.debug = debug;
		p.cover = c;
	}
}

// The closest distance between the eye ray (ro + rd t, rd unit length) and the segment a-b; t: where along the ray.
float RaySegment(float3 ro, float3 rd, float3 a, float3 b, out float t) {
	float3 ab = b - a;
	float3 w0 = ro - a;
	float bb = dot(rd, ab);
	float cc = dot(ab, ab);
	float dd = dot(rd, w0);
	float ee = dot(ab, w0);
	float denom = cc - bb * bb;
	float u = denom > 1e-6 ? saturate((ee - bb * dd) / denom) : 0.0;
	t = dot(a + ab * u - ro, rd);
	return length(ro + rd * t - (a + ab * u));
}

// UNOFFICIAL 2026-10-13 rewrite: the loop over an item's streams only finds the stream a pixel is nearest to (FindPath,
// which also draws the puddles and hanging drops at the streams' ends); that stream is drawn once afterwards
// (PaintStream). The flowing stream did not fit inside the loop in ps_3_0's 32 registers, and only one is needed: where
// two streams overlap, the nearer one's edge wins.
struct Nearest {
	float row;		// its row in the path texture (-1: none)
	float best;		// how far the pixel is from it (from the part run so far)
	float arc;		// how far along it that nearest point is
	float side;		// which side of it the pixel is (its sign; seen along the surface's normal n)
	float score;	// how far outside its width: the smallest wins
};

// A stream's state at `age`: how much of it runs over the ground rather than down a wall (a wall's path drops about its
// whole length; one down a 10 degree slope only a sixth of it), how much of its path is shown, how much has run past its
// end (it gathers or drips there), its longest run. UNOFFICIAL 2026-10-13 (user screenshot: on the ground the streams were
// stiff even bars with a round shiny cap at the end, "like mushrooms"): on the ground it is thinner and three times slower
// (it soaks in as it goes), tapers to a point and soaks a faint stain into the dirt; what keeps coming ends as a small
// flat smear. Walls as before.
void StreamState(float row, inout Stream s, float age, out float ground, out float shown, out float excess, out float lengthMax) {
	float4 head = PathTexel(0.0, row);
	float4 last = PathTexel(1.0, row);
	lengthMax = max(last.w, 0.01);
	// UNOFFICIAL 2026-10-14: the amount of blood the probe found where the stream starts (its longest run over the one it would
	// have had; 1 for pools and without the splatter texture): more blood makes a wider stream with a bigger bead, that starts
	// sooner (it sags off the splat at once); a little makes a thin, late trickle.
	float amount = clamp(lengthMax / max(s.lengthMax, 0.01), 0.35, 1.4);
	s.width *= pow(amount, 0.45);
	s.delay *= pow(amount, -0.7);
	s.dropVolume *= pow(amount, 0.45);
	float3 chord = last.xyz - PathTexel(2.0, row).xyz;
	ground = 1.0 - smoothstep(0.45, 0.75, abs(chord.z) / max(length(chord), 1e-3));
	s.tau *= lerp(1.0, 3.0, ground);
	s.delay *= lerp(1.0, 2.0, ground);
	// UNOFFICIAL 2026-10-14 (user: a splat of paint flowing down the terrain): on the ground the blood leaves the splat as a
	// tongue, two to three times a wall drip's width (wider with more blood), not a thin thread.
	s.width = lerp(s.width, min(s.width * lerp(1.8, 3.2, saturate((amount - 0.35) / 1.05)), 5.0), ground);
	float run = StreamRun(s, lengthMax, age);
	shown = min(run, head.y);
	excess = max(run - head.y, 0.0);
}

// Where a stream's front is (`shown` along its path), item-relative.
float3 StreamTip(float row, float shown, float lengthMax) {
	float f = min(shown / (lengthMax / 13.0), 13.0);
	float4 P0 = PathTexel(2.0 + floor(f), row);
	float4 P1 = PathTexel(2.0 + min(floor(f) + 1.0, 13.0), row);
	return lerp(P0.xyz, P1.xyz, saturate((shown - P0.w) / max(P1.w - P0.w, 1e-4)));
}

// One stream (path texture row `row`): how near the pixel is to it (kept in nr if nearest so far), and the puddle it ends
// in or the drop hanging at the edge it reached. pp: the pixel's surface point, item-relative.
void FindPath(float row, Stream s, float3 pp, float3 n, float aa, float age, float seed, inout Nearest nr, inout Paint p) {
	float4 head = PathTexel(0.0, row);
	if (head.w < -0.5) return;
	float ground, shown, excess, lengthMax;
	StreamState(row, s, age, ground, shown, excess, lengthMax);
	if (shown + excess < 0.05) return;
	// Is the pixel near the path at all? A path of length L from its start to its end strays at most
	// sqrt(L^2 - chord^2) / 2 from the straight line between them.
	float3 first = PathTexel(2.0, row).xyz;
	float3 chord = PathTexel(1.0, row).xyz - first;
	float chordLength2 = dot(chord, chord);
	float3 toLine = pp - (first + chord * saturate(dot(pp - first, chord) / max(chordLength2, 1e-4)));
	float stray = 0.5 * sqrt(max(head.y * head.y - chordLength2, 0.0));
	if (length(toLine) > stray + s.width * 3.0 + 2.0 + sqrt(excess * s.width) + aa) return;
	// The points are spaced lengthMax / 13 apart along the path (until it stops): the closest point on the part run so far.
	float segments = min(ceil(shown * 13.0 / lengthMax - 0.001), 13.0);
	float best = 1e9;
	float bestArc = 0.0;
	float bestSide = 1.0;
	float4 A = PathTexel(2.0, row);
	[loop] for (float j = 0.0; j < segments; j += 1.0) {
		float4 B = PathTexel(j + 3.0, row);
		float3 ab = B.xyz - A.xyz;
		float l2 = dot(ab, ab);
		float most = saturate((shown - A.w) / max(B.w - A.w, 1e-4));
		float t = l2 > 1e-6 ? min(saturate(dot(pp - A.xyz, ab) / l2), most) : 0.0;
		float3 off = pp - (A.xyz + ab * t);
		float dist = length(off);
		if (dist < best) { best = dist; bestArc = lerp(A.w, B.w, t); bestSide = dot(cross(ab, off), n); }
		A = B;
	}
	if (best - s.width * 1.5 < nr.score) {
		nr.row = row;
		nr.best = best;
		nr.arc = bestArc;
		nr.side = bestSide;
		nr.score = best - s.width * 1.5;
	}
	[branch] if (excess > 0.0) {
		float3 toTip = pp - StreamTip(row, shown, lengthMax);
		[branch] if (head.w < 1.5) {
			// Gathered where it stopped: a puddle as big as what kept coming (on the ground it soaks in: smaller, flat and
			// matte). UNOFFICIAL 2026-10-13 (WetWorld's puddles in hollows): it reaches further where the surface is lower than
			// where the stream stopped and stays off bumps; its edge ragged by the noise (more with Viscosity).
			float rp = s.width * 1.2 + sqrt(excess * s.width * lerp(0.9, 0.3, ground));
			float rag = FlowNoise(float3(toTip.xy / max(rp, 0.5) * 1.3, seed * 23.0 + row)).x - 0.5;
			float T = 1.0 - dot(toTip.xy, toTip.xy) / (rp * rp) + rag * (0.5 + 0.5 * TESR_BloodFlowFlow.y) * lerp(0.6, 0.9, ground);
			// With the user's splatter decals: their shape (a whole middle, ragged edge, droplets), a smooth core kept.
			[branch] if (TESR_BloodFlowProbe.w > 0.5)
				T = Splatter(toTip.xy / (rp * 2.6), seed + row * 0.137, aa / (rp * 2.6)) * 1.25 - 0.1 + RaggedCore(toTip.xy, rp * 0.5, seed + row * 0.37);
			T += clamp(-toTip.z / max(rp * 0.25, 0.4), -1.5, 1.0) * 0.45;
			Add(p, Shows(T, aa * 2.0 / rp) * step(abs(toTip.z), rp + 2.0), lerp(0.55 + 0.35 * saturate(T), 0.3, ground), float3(toTip.xy / rp, 0.0) * saturate(1.0 - 3.0 * T) * 0.4 * (1.0 - ground), 2.0);
		}
		else {
			// Hanging at the edge, swelling until it lets go.
			float rh = s.width * (1.0 + 0.9 * pow(frac(excess / s.dropVolume), 0.333));
			float3 off = toTip + float3(0.0, 0.0, rh * 0.5);
			Add(p, saturate(0.5 - (length(off) - rh) / aa), 0.9, off / rh, 3.0);
		}
	}
}

// The stream nearest the pixel (nr, from FindPath; s: that stream's), drawn. UNOFFICIAL 2026-10-13 rewrite: a thickness
// field (rounded across, thickest in the middle), with the rain-streak texture flowing down it and noise bunching it; what
// shows is where it is thicker than BloodAmount's threshold (Shows), so it breaks up into clumps and beads rather than
// thinning evenly.
void PaintStream(Nearest nr, Stream s, float3 pp, float3 n, float aa, float age, float seed, inout Paint p) {
	[branch] if (nr.row < 0.0) return;
	float row = nr.row;
	float ground, shown, excess, lengthMax;
	StreamState(row, s, age, ground, shown, excess, lengthMax);
	float3 way = PathTexel(1.0, row).xyz - PathTexel(2.0, row).xyz;
	way *= rsqrt(max(dot(way, way), 1e-6));
	float along = saturate(nr.arc / max(shown, 0.01));
	// How far behind the front (ahead of it: < 0).
	float back = nr.arc < shown - 0.01 ? shown - nr.arc : -dot(pp - StreamTip(row, shown, lengthMax), way);
	// Its width: full behind the front, thinner toward its start (only a coat is left there); the front swells into a bead
	// while it runs (on the ground hardly: it soaks in as it goes, tapering to a point).
	float bead = saturate(1.0 - back / (3.0 * s.width)) * (excess <= 0.0 ? 1.0 : 0.4);
	// (UNOFFICIAL 2026-10-14: on the ground a tongue keeps its width and ends in a lumpy lobe, as paint does, instead of tapering
	// to a point.)
	float w = max(s.width * lerp(0.6, 1.0, along) * lerp(1.0, 0.85 + 0.15 * along, ground) * (1.0 + lerp(0.45, 0.6, ground) * bead), 0.05);
	float3 side = normalize(cross(n, way));
	float u = (nr.side < 0.0 ? -nr.best : nr.best) / w;	// across it, in its half widths (+: on the `side` side)
	// Fluid motion (WetWorld's scrolling streaks as a flow map): the streak texture creeps down the stream in two phases half a
	// period apart, each sliding further every second and crossfaded so neither stretches for ever. How fast each part creeps
	// and how far it drifts sideways comes from the distortion noise, which itself creeps slowly down the stream; Viscosity
	// scales it: thicker blood moves more unevenly and bunches into clumps.
	float2 dn = FlowNoise(float3(u * 0.6, nr.arc * 0.3 - TESR_BloodFlowFlow.w * TESR_BloodFlowFlow.x * 0.1, seed * 37.0 + row * 0.37)) - 0.5;
	float phase = frac(TESR_BloodFlowFlow.w / FlowPeriod + seed);
	float2 drift = float2(dn.y * TESR_BloodFlowFlow.y * StreakAcross * 0.5,
		-CreepUnits * FlowPeriod * StreakAlong * TESR_BloodFlowFlow.x * max(1.0 + 1.6 * TESR_BloodFlowFlow.y * dn.x, 0.1) * lerp(1.0, 0.4, ground));
	float4 st = float4(u * 0.5 * StreakAcross + frac(seed * 7.3 + row * 0.29), nr.arc * StreakAlong, 0.0, max(log2(aa * StreakAlong * 1024.0), 0.0));
	float4 t = lerp(tex2Dlod(TESR_BloodStreaks, st + float4(drift * phase, 0.0, 0.0)),
		tex2Dlod(TESR_BloodStreaks, st + float4(drift * frac(phase + 0.5) + 0.5, 0.0, 0.0)), abs(1.0 - 2.0 * phase)).gbar;
	// (Clumps: the noise also swells and pinches it along its length, more with Viscosity.)
	float T = 1.0 - u * u * (1.0 - 0.6 * TESR_BloodFlowFlow.y * dn.y) + (t.a - 0.3) * 0.5 + dn.x * TESR_BloodFlowFlow.y * 0.7 * (1.0 + bead);
	// Its surface: rounded across, rippled by the flowing streaks (moving glints), leaning forward on the bead.
	Add(p, Shows(T, aa * 2.0 / w), saturate(T) * lerp(lerp(0.35, 0.9, bead), lerp(0.4, 0.75, bead), ground),
		(side * (u * 0.6 + (t.x * 2.0 - 1.0) * 0.5 * t.a) + way * (saturate(-back / w) * 0.5 - (t.y * 2.0 - 1.0) * 0.5 * t.a)) * lerp(1.0, 0.5, ground), 1.0);
	[branch] if (ground > 0.01) {
		float soak = s.width * 2.6 + 0.6;
		Add(p, saturate(1.0 - nr.best / soak) * 0.3 * ground * (1.0 - 0.5 * along), 0.0, 0.0, 1.0);
	}
}

// One stream's drops, if it runs off an edge: one leaves each time `dropVolume` more has run, falling as anything
// falls (drawn on the eye ray ro + rd t in front of the surface at sceneDist), and the stain where they land.
void PaintDrops(float row, Stream s, float3 pp, float3 ro, float3 rd, float sceneDist, float aa, float age, float seed, inout Paint p) {
	float4 head = PathTexel(0.0, row);
	if (head.w < 1.5) return;
	float4 last = PathTexel(1.0, row);
	float lengthMax = max(last.w, 0.01);
	float excess = StreamRun(s, lengthMax, age) - head.y;
	if (excess < s.dropVolume) return;
	float3 E = last.xyz;		// the edge
	float fallLength = head.z > 0.0 ? head.z : TESR_BloodFlowPool.z;
	// Nowhere near the column the drops fall down, nor the stain under it: nothing to do.
	float tColumn;
	float nearColumn = RaySegment(ro, rd, E - float3(0.0, 0.0, fallLength), E, tColumn) - s.width * 1.5 - aa * tColumn / sceneDist;
	float nearStain = length(pp - E + float3(0.0, 0.0, fallLength)) - s.width * 10.0;
	if (nearColumn > 0.0 && nearStain > 0.0) return;
	float g = TESR_BloodFlowPool.w;
	float fallTime = sqrt(2.0 * fallLength / g);
	float fallen = floor(excess / s.dropVolume);
	float radius = s.width * 1.15;
	[loop] for (float m = 0.0; m < 2.0; m += 1.0) {
		float since = age - StreamTime(s, lengthMax, head.y + (fallen - m) * s.dropVolume);
		if (fallen - m < 1.0 || since < 0.0 || since > fallTime) continue;
		float3 bottom = E - float3(0.0, 0.0, 0.5 * g * since * since + radius);
		float t;
		float dist = RaySegment(ro, rd, bottom, bottom + float3(0.0, 0.0, min(g * since * 0.02, 8.0)), t);	// a little stretched by its speed
		if (t > 0.0 && t < sceneDist - 0.5) p.air = max(p.air, saturate(0.5 - (dist - radius) / max(aa * t / sceneDist, 0.02)));
	}
	// Where they land: a stain that grows with each drop, with a few spatters round it.
	float landed = floor(max(StreamRun(s, lengthMax, age - fallTime) - head.y, 0.0) / s.dropVolume);
	[branch] if (head.z > 0.0 && landed >= 1.0) {
		float3 off = pp - E + float3(0.0, 0.0, head.z);
		float rl = s.width * (1.2 + 0.9 * sqrt(landed));
		float c;
		// UNOFFICIAL 2026-10-13 (user: the little splatters should be a small copy of my blood texture, not a blot): with the
		// splatter texture, the stain is one of its splatters (a whole middle kept), growing with each drop.
		[branch] if (TESR_BloodFlowProbe.w > 0.5) {
			float T = SmallSplatter(off.xy / (rl * 3.2), seed + row * 0.29, aa / (rl * 3.2)) * 1.25 - 0.1 + saturate(1.0 - length(off.xy) / (rl * 0.35)) * 0.5;
			c = Shows(T, aa * 2.0 / rl);
		}
		else {
			float edge = rl * (0.75 + 0.5 * valueNoise(float2(atan2(off.y, off.x) * 2.0 + seed * 40.0, row)));
			c = saturate(0.5 - (length(off) - edge) / aa);
			float cellSize = s.width * 1.5;
			float2 inCell = frac(off.xy / cellSize) - 0.5;
			float spot = hash2(floor(off.xy / cellSize) + seed * 13.0) > 0.8 ? saturate(0.5 - (length(inCell) * cellSize - s.width * 0.35) / aa) : 0.0;
			c = max(c, spot * step(length(off), rl * 2.2) * saturate(landed / 3.0));
		}
		Add(p, c * step(abs(off.z), 3.0), 0.55, float3(0.0, 0.0, 0.0), 4.0);
	}
}

// UNOFFICIAL 2026-10-13: a highlight off a smooth liquid (GGX, roughness^4 = a2; Fresnel of a watery liquid, 2 % head on).
// Its brightness is set by the caller.
float Glint(float3 N, float3 V, float3 L, float a2) {
	float3 H = normalize(L + V);
	float nh = saturate(dot(N, H));
	float lh = saturate(dot(L, H));
	float d = nh * nh * (a2 - 1.0) + 1.0;
	return a2 / (3.14159 * d * d) * (0.02 + 0.98 * pow(1.0 - lh, 5.0)) * 0.25 / max(lh * lh, 0.1) * saturate(dot(N, L));
}

// A lamp's share of the highlight: lamp.xyz in world space, lamp.w its radius (0: no lamp). The fall-off is WetWorld's.
float LampGlint(float3 N, float3 V, float3 P, float4 lamp, float3 color, float a2) {
	if (lamp.w <= 0.0) return 0.0;
	float3 toLamp = lamp.xyz - P;
	float s = saturate(dot(toLamp, toLamp) / (lamp.w * lamp.w));
	if (s >= 1.0) return 0.0;		// out of its reach
	float atten = (1.0 - s) * (1.0 - s) / (1.0 + 5.0 * s);
	return Glint(N, V, normalize(toLamp), a2) * atten * saturate(luma(color));
}

float4 BloodPS(PSIN IN) : COLOR0 {
	float2 uv = (IN.vpos + 0.5) * TESR_ReciprocalResolution.xy;
	float viewmodelDepth = tex2D(TESR_DepthBufferViewModel, uv).x;
	if ((TESR_DepthConstants.z == 0 && viewmodelDepth < 0.9) || (TESR_DepthConstants.z > 0 && viewmodelDepth > 0.01)) discard;
	float depth = min(readDepth(uv), farZ * 0.98);
	bool sky = depth >= farZ * 0.979;

	// (Kept to what the streams need: the rest is worked out after them, so the shader fits in its registers.)
	float slot, pool, fresh;
	DecodeSlot(IN.a.w, slot, pool, fresh);
	float3 rel = toWorld(uv) * depth;		// the surface, eye-relative
	float sceneDist = length(rel);
	float3 rd = rel / sceneDist;
	// The surface, item-relative (none on the sky); the eye is at -IN.a.xyz.
	float3 pp = sky ? float3(1e5, 1e5, 1e5) : rel - IN.a.xyz;
	// Rounded: values passed from the vertices differ a little from pixel to pixel, and the hashes would turn that into
	// noise (drips flickering on and off from row to row).
	float seed = ItemSeed(IN.c.z);
	// The size of a pixel on the surface here (for smooth edges).
	float aa = max(depth * TESR_ReciprocalResolution.y * 2.0 / TESR_ProjectionTransform[1][1], 0.05);
	float firstRow = slot * Streams;

	// Outside the box round everything this item's blood reaches (the bounds pass), and the eye ray not crossing it
	// either (a drop could be falling in front of the surface): nothing here.
	float boundsV = (firstRow + Streams * 0.5) * TESR_BloodFlowProbe.x;	// its row of the bounds texture (one per slot)
	float4 lo = tex2Dlod(TESR_BloodFlowBounds, float4(0.25, boundsV, 0.0, 0.0));
	float4 hi = tex2Dlod(TESR_BloodFlowBounds, float4(0.75, boundsV, 0.0, 0.0));
	[branch] if (lo.w > 0.5 && pool < 0.5) {	// (2026-10-20: not for pools, as CutBox)
		float3 margin = aa + 0.5;
		bool inside = all(pp >= lo.xyz - margin) && all(pp <= hi.xyz + margin);
		float3 inverse = 1.0 / (abs(rd) > 1e-6 ? rd : 1e-6);
		float3 t0 = (lo.xyz - margin + IN.a.xyz) * inverse;
		float3 t1 = (hi.xyz + margin + IN.a.xyz) * inverse;
		float3 tMin = min(t0, t1);
		float3 tMax = max(t0, t1);
		float tNear = max(max(tMin.x, tMin.y), tMin.z);
		float tFar = min(min(tMax.x, tMax.y), tMax.z);
		if (!inside && !(tNear <= tFar && tFar > 0.0 && tNear < sceneDist)) discard;
	}

	Paint p;
	p.cover = 0.0;
	p.thick = 0.0;
	p.bend = 0.0;
	p.air = 0.0;
	p.debug = 0.0;
	float rim = 0.0;
	float poolShape = 0.0, poolKeep = 1.0;	// (debug view 2)

	[branch] if (pool > 0.5) {
		// The ground round the pool (from the probe): on a slope the pool moves downhill and stretches that way.
		float4 groundHead = PathTexel(0.0, firstRow + Streams - 1.0);
		float4 ground = PathTexel(1.0, firstRow + Streams - 1.0);
		bool known = fresh < 0.5 && groundHead.y >= 5.0 && TESR_BloodFlowGround.w < 0.5;
		float3 gN = known ? ground.xyz : float3(0.0, 0.0, 1.0);
		float slope = length(gN.xy) / max(gN.z, 0.3);
		float2 downhill = slope > 1e-3 ? normalize(gN.xy) : float2(1.0, 0.0);
		float R = IN.b.w;
		float grow = sqrt(saturate(1.0 - exp(-IN.c.x * 3.0 / TESR_BloodFlowPool.x)));	// most of the way by the spread time
		float radius = R * grow;
		float2 q = pp.xy - downhill * R * PoolShift(slope) * grow;
		float alongQ = dot(q, downhill);
		float2 shaped = float2(alongQ / (alongQ > 0.0 ? PoolStretch(slope) : 1.0), dot(q, float2(-downhill.y, downhill.x)));
		float fromCentre = length(shaped);
		// On the ground near the plane the probe found (a little more leeway further out).
		float groundZ = (known ? ground.w : 0.0) - (gN.x * pp.x + gN.y * pp.y) / max(gN.z, 0.3);
		float3 Nw = normalize(GetWorldNormalLod(uv));
		float keep;
		[branch] if (TESR_BloodFlowGround.x > 0.5) {
			// UNOFFICIAL 2026-10-16 (GroundFit, after Terrain blending's soft edge): the pool fades out over a few pixels where
			// the surface leaves the ground instead of stopping on a hard line, and faces are judged against the ground's own
			// slope, not straight up. The height over the ground: the probe's plane when it found the ground round the pool;
			// else (the ring hidden, by the body, grass or the screen's edge) the surface here, its plane carried to the
			// pool's centre: on even ground that is the same height at every pixel, whatever the slope (the old fallback, a
			// level plane through the body's lowest bone, cut a straight line across the pool's uphill side).
			// UNOFFICIAL 2026-10-20 (user screenshots: half-disc pools, flat dark, cut on a straight line through the middle): the
			// probe's plane is often tilted by grass or the body in its ring, so half the pool counted as a hollow (filled solid out
			// to its whole reach) and half as a bump (cut). The surface's own plane here is used instead, always.
			bool sure = false;
			// (2026-10-20: height over the pool's centre, not the pixel's own plane, which is noisy and flickered with the view;
			// the room below grows with the slope here.)
			float h = sure ? pp.z - groundZ : pp.z;
			float tiltRoom = fromCentre * sqrt(saturate(1.0 - Nw.z * Nw.z)) / max(Nw.z, 0.5);
			float3 up = sure ? normalize(float3(gN.xy, max(gN.z, 0.3))) : float3(0.0, 0.0, 1.0);
			float fade = max(aa * TESR_BloodFlowGround.y, 1.5);
			keep = saturate((5.0 + 0.15 * fromCentre + tiltRoom + fade - h) / fade) * saturate((8.0 + 0.3 * fromCentre + tiltRoom + fade + h) / fade)
				* saturate((dot(Nw, up) - 0.45) / 0.25);
			groundZ = pp.z - h;		// (the hollows below read the same height)
		}
		else keep = (pp.z < groundZ + 5.0 + 0.15 * fromCentre && pp.z > groundZ - (8.0 + 0.3 * fromCentre) && Nw.z > 0.6) ? 1.0 : 0.0;
		poolKeep = keep;
		[branch] if (fromCentre < R * 1.45 && radius > 0.05 && (keep > 0.0 || TESR_BloodFlowInfo.y > 1.5)) {
			// A main body and a lobe off one side, joined smoothly, with a ragged edge from noise fixed to the ground.
			float angle = seed * 6.2832;
			float2 lobeWay = float2(cos(angle), sin(angle));
			float dMain = fromCentre - radius;
			float dLobe = length(shaped - lobeWay * radius * 0.55) - radius * 0.6;
			float kk = radius * 0.3 + 0.01;
			float mixLobe = saturate(0.5 + 0.5 * (dLobe - dMain) / kk);
			float dist = lerp(dLobe, dMain, mixLobe) - kk * mixLobe * (1.0 - mixLobe);
			dist += (fbm(pp.xy * 0.09 + seed * 17.0) - 0.5) * radius * 0.55 * (0.5 + 0.5 * TESR_BloodFlowFlow.y);
			// UNOFFICIAL 2026-10-13 (WetWorld's puddles in hollows): ground lower than the plane the probe found fills first,
			// bumps stay dry; then as a thickness (0.25 at the edge it had before), shown above BloodAmount's threshold.
			dist += clamp(pp.z - groundZ, -radius * 0.12, radius * 0.4) * 0.8;	// (2026-10-20: a hollow deepens blood, never makes it)
			float T = 0.25 - dist / max(radius, 0.5) * 0.9;
			// With the user's splatter decals: the pool takes one's shape (stretched downhill with it), its dense middle the
			// deep pool, its spray and droplets round the edge; a smooth core keeps the middle whole, hollows and bumps as above.
			[branch] if (TESR_BloodFlowProbe.w > 0.5)
				T = Splatter(shaped / (radius * 2.2), seed, aa / (radius * 2.2)) * 1.25 - 0.1 + RaggedCore(shaped, radius * 0.55, seed)
					- clamp(pp.z - groundZ, -radius * 0.12, radius * 0.4) * 0.72 / max(radius, 0.5);
			float cover = Shows(T, aa * 0.9 / max(radius, 0.5));
			poolShape = cover;
			cover *= keep;
			float inward = (T - 1.0 + TESR_BloodFlowFlow.z) * max(radius, 0.5) / 0.9;	// units inside its shown edge
			rim = saturate(1.0 - inward / max(radius * 0.12, 0.8)) * cover;
			float deep = saturate(inward / max(radius * 0.5, 0.5));
			// The edge bulges (a meniscus): its normal leans outward there.
			float2 outward = length(pp.xy) > 0.001 ? normalize(pp.xy) : float2(0.0, 0.0);
			Add(p, cover, 0.35 + 0.65 * deep, float3(outward * rim * rim * 0.3, 0.0), 2.0);
		}
		// Trickles off the downhill side.
		Nearest nr;
		nr.row = -1.0; nr.best = 1e9; nr.arc = 0.0; nr.side = 1.0; nr.score = 1e9;
		[loop] for (float k = 0.0; k < 2.0; k += 1.0)
			FindPath(firstRow + k, PoolStream(seed, k, IN.b.w, slope, TESR_BloodFlowPool.x), pp, float3(gN.xy, max(gN.z, 0.3)), aa, IN.c.x, seed, nr, p);
		PaintStream(nr, PoolStream(seed, nr.row - firstRow, IN.b.w, slope, TESR_BloodFlowPool.x), pp, float3(gN.xy, max(gN.z, 0.3)), aa, IN.c.x, seed, p);
		[loop] for (float k2 = 0.0; k2 < 2.0; k2 += 1.0)
			PaintDrops(firstRow + k2, PoolStream(seed, k2, IN.b.w, slope, TESR_BloodFlowPool.x), pp, -IN.a.xyz, rd, sceneDist, aa, IN.c.x, seed, p);
	}
	else {
		Nearest nr;
		nr.row = -1.0; nr.best = 1e9; nr.arc = 0.0; nr.side = 1.0; nr.score = 1e9;
		[loop] for (float k = 0.0; k < Streams; k += 1.0)
			FindPath(firstRow + k, SplatStream(seed, k, IN.b.w, IN.c.w, TESR_BloodFlowInfo.w), pp, IN.b.xyz, aa, IN.c.x, seed, nr, p);
		PaintStream(nr, SplatStream(seed, nr.row - firstRow, IN.b.w, IN.c.w, TESR_BloodFlowInfo.w), pp, IN.b.xyz, aa, IN.c.x, seed, p);
		[loop] for (float k2 = 0.0; k2 < Streams; k2 += 1.0)
			PaintDrops(firstRow + k2, SplatStream(seed, k2, IN.b.w, IN.c.w, TESR_BloodFlowInfo.w), pp, -IN.a.xyz, rd, sceneDist, aa, IN.c.x, seed, p);
	}

	float4 scene = tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0, 0.0));
	float splat = 0.0;		// the game's own splat under it (found by its red)
	if (pool < 0.5) {
		// The game's splat: inside its radius, wherever the image is red.
		float inside = 1.0 - smoothstep(0.7, 1.05, length(pp) / IN.b.w);
		float redness = saturate((scene.r - max(scene.g, scene.b)) / max(scene.r, 0.02) * 1.6 - 0.35);
		splat = inside * redness;
		p.cover *= 1.0 - 0.6 * splat * step(p.thick, 0.5);	// where a trail starts inside the splat, the splat shows
	}

	[branch] if (TESR_BloodFlowInfo.y > 1.5) {
		// UNOFFICIAL 2026-10-16 debug view 2 (the cuts): where the pool's shape has blood, magenta if the ground test takes it
		// away, orange if it only thins it (a soft edge), yellow if it shows.
		if (poolShape > 0.5) return poolKeep < 0.05 ? float4(1, 0, 1, 0.9) : poolKeep < 0.95 ? float4(1, 0.5, 0, 0.9) : float4(1, 0.9, 0, 0.85);
		discard;
	}
	if (TESR_BloodFlowInfo.y > 0.5) {
		// Debug view. Red: trails; yellow: puddles and pools; blue: a drop at an edge; cyan: where drops land; white:
		// falling drops; green: the rest of the area it looks at; white rings every 10 units round the item's centre.
		if (p.air > 0.5) return float4(1, 1, 1, 0.9);
		if (p.cover > 0.5) {
			float3 c = p.debug < 1.5 ? float3(1, 0, 0) : p.debug < 2.5 ? float3(1, 0.9, 0) : p.debug < 3.5 ? float3(0, 0.3, 1) : float3(0, 1, 1);
			return float4(c, 0.85);
		}
		if (pp.x < 9e4 && frac(length(pp) / 10.0) < 0.06) return float4(1, 1, 1, 0.6);
		return float4(0, 0.6, 0, 0.35);
	}
	float amount = saturate(max(max(p.cover, splat), p.air));
	if (amount * IN.c.y < 0.004) discard;

	float3 sceneLin = linearize(max(scene.rgb, 0.0));
	// The light here, from the brightest channel (the game's red splat is dark only in green and blue); capped, so a
	// lamp-lit or white surface cannot make the blood glow.
	// UNOFFICIAL 2026-10-13 (user screenshot: our blood far darker than the game's on dark asphalt): the light here from how
	// bright the surface looks assumes a mid-grey surface, so on dark ground it came out a few times too low (the game lights
	// its decals by the real light). Now also the light worked out from the sky (ambient) and the sun (facing it, not in
	// shadow), the brighter of the two counting.
	float lit = 0.0;
	if (TESR_BloodFlowInfo.x > 0.5) {
		float sunAmbient = max(luma(linearize(TESR_SunAmbient.rgb)), 0.001);
		float4 shadows = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0, 0.0));
		lit = saturate(pow(abs((shadows.r + shadows.g) / sunAmbient), 5.0));
	}
	float irradiance = luma(linearize(saturate(TESR_SunAmbient.rgb)))
		+ lit * luma(linearize(saturate(TESR_SunColor.rgb))) * saturate(dot(normalize(GetWorldNormalLod(uv)), normalize(TESR_SunDirection.xyz)));
	float light = min(max(max(sceneLin.r, max(sceneLin.g, sceneLin.b)) / AssumedAlbedo, irradiance), 2.5);
	float age = IN.c.x;
	float dryTime = pool > 0.5 ? TESR_BloodFlowInfo.z : TESR_BloodFlowDry.w;
	// Drying: a thin film dries much sooner than a thick layer.
	float dry = smoothstep(0.0, 1.0, age / (dryTime * lerp(0.5, 1.6, p.thick)));
	// UNOFFICIAL 2026-10-13 (user: match my regular blood's colour): the game draws its splat as the grey splatter texture
	// times the decal's colour; the blood takes that (MatchGameColor), dried darker and browner as the settings' colours say.
	float3 freshColour = TESR_BloodFlowFresh.rgb;
	float3 dryColour = TESR_BloodFlowDry.rgb;
	float3 glintTint = float3(0.02, 0.004, 0.004);
	[branch] if (TESR_BloodFlowMatch.w > 0.5 && IN.d.w > 0.5) {
		float t = floor(IN.d.w + 0.5);
		float r = floor(t / 65536.0);
		r += (t - r * 65536.0 >= 65536.0) ? 1.0 : ((t - r * 65536.0 < 0.0) ? -1.0 : 0.0);
		float gb = t - r * 65536.0;
		float g = floor(gb / 256.0);
		g += (gb - g * 256.0 >= 256.0) ? 1.0 : ((gb - g * 256.0 < 0.0) ? -1.0 : 0.0);
		freshColour = linearize(saturate(float3(r, g, gb - g * 256.0) / 255.0 * TESR_BloodFlowMatch.x)) * TESR_BloodFlowMatch.y;
		// (UNOFFICIAL 2026-10-27, MatchFloor: no darker than TESR_BloodFlowLook.w in its strongest channel, the hue kept. The
		// user's splatter grey x decal colour came to about 0.007 linear red: black, so the sheen on top read grey.)
		freshColour *= max(1.0, TESR_BloodFlowLook.w / max(max(freshColour.r, max(freshColour.g, freshColour.b)), 1e-4));
		// (Dried: a bit darker and a touch browner. The settings' dried/fresh ratio was made for FreshColor's bright red and
		// took the game's own dark red down to a third, near black: user, 2026-10-13, old pools far darker than the decals.)
		dryColour = freshColour * float3(0.72, 0.8, 0.8) + freshColour.r * float3(0.0, 0.1, 0.05);
		glintTint = 0.004 + 0.016 * freshColour / max(max(freshColour.r, max(freshColour.g, freshColour.b)), 1e-4);	// (the bead's tint in the blood's own hue)
	}
	float3 albedo = lerp(freshColour, dryColour, dry);
	// Dried blood is uneven: thicker and thinner patches, fixed to the surface.
	albedo *= lerp(1.0, 0.8 + 0.4 * valueNoise((pp.xy + pp.z) * 0.35 + seed * 31.0), dry);
	// UNOFFICIAL 2026-10-13: the sub-surface look. Light goes into the blood, down to what it is on and back out (twice its
	// depth: 0.25 for the thinnest film to 2.25 for a deep layer); blood lets red through and soaks up green and blue (from
	// its colour: each channel's extinction is 1.2 plus how much darker it is than red), so a thin film tints what it is on
	// a clear, saturated red and a thick layer shows only the light scattered inside it: the blood's own deep red, darker
	// where deep, with a dark ring at a dried pool's edge.
	// (UNOFFICIAL 2026-10-25: light passes the blood in its own colour, measured from its strongest channel. It was from red,
	// which made green, yellow or other matched blood glow or go red; for red blood the strongest channel is red: unchanged.)
	float3 through = exp(-(1.2 + log(max(max(albedo.r, max(albedo.g, albedo.b)), 0.001) / max(albedo, 0.001))) * (0.25 + 2.0 * p.thick));
	float3 blood = (sceneLin * through + light * albedo * lerp(1.0, 0.6, saturate(p.thick * 1.3 - 0.3)) * (1.0 - through)) * (1.0 - 0.35 * dry * rim);
	// Outdoors, where the sun reaches: looking toward the sun, it shines through thin fresh blood (a red glow at edges and
	// in thin films; no more than the light here).
	blood += light * albedo * (pow(saturate(dot(rd, normalize(TESR_SunDirection.xyz))), 4.0) * lit * (1.0 - dry) * (1.0 - saturate(p.thick)) * 0.8);
	float3 colour = sceneLin;
	colour = lerp(colour, colour * lerp(1.0, float3(0.5, 0.62, 0.62), dry), splat);	// the game's splat dries too: darker, browner
	colour = lerp(colour, blood, p.cover);

	// Fresh blood is smooth and glossy, thick blood most: sharp highlights of the sun (outdoors, where it is not shaded)
	// and the lamps, at most about twice the light measured here (no glare), rougher as it dries; at low angles it
	// reflects the sky outdoors, the room's light indoors.
	// UNOFFICIAL 2026-10-13 (user screenshot: a gold speckled band where low sun fell on a big splat): the game's own splat is found
	// by its red, pixel by pixel, so it is as speckled as its texture, and at half gloss every speckle caught the sun (the scene's
	// warm grade then turned the highlights gold). It keeps only a faint sheen now; the beads (BeadsPS) carry its wetness.
	float wet = max(p.cover, splat * 0.12);
	float gloss = TESR_BloodFlowFresh.w * (1.0 - dry) * wet * lerp(0.5, 1.0, p.thick);
	[branch] if (gloss > 0.001) {
		float3 V = -rd;
		float3 Nb = normalize(normalize(GetWorldNormalLod(uv)) + p.bend * 0.8);
		float rough = lerp(0.12, 0.45, dry);
		float a2 = rough * rough * rough * rough;
		float glints = 0.0;
		if (TESR_BloodFlowInfo.x > 0.5) glints += Glint(Nb, V, normalize(TESR_SunDirection.xyz), a2) * lit * saturate(luma(linearize(TESR_SunColor.rgb)));
		float3 P = TESR_CameraPosition.xyz + rd * sceneDist;
		[loop] for (int i = 0; i < 12; i++) {
			glints += LampGlint(Nb, V, P, TESR_ShadowLightPosition[i], TESR_LightColor[i].rgb, a2);
			glints += LampGlint(Nb, V, P, TESR_LightPosition[i], TESR_LightColor[i + 12].rgb, a2);
		}
		float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(Nb, V)), 5.0);
		float3 surroundings = TESR_BloodFlowInfo.x > 0.5 ? linearize(saturate(TESR_HorizonColor.rgb)) : light * 0.1;
		// (UNOFFICIAL 2026-10-27, TESR_BloodFlowLook: the sheen weaker, capped, and part in the blood's own hue, from glintTint:
		// (glintTint - 0.004) / 0.016 is the colour over its strongest channel, (1, 0, 0) for FreshColor. OldLook: as before.)
		float3 sheenTint = lerp(1.0, saturate((glintTint - 0.004) / 0.016), TESR_BloodFlowLook.z);
		// (Capped: no more than about the light here, so a low sun cannot turn a wide patch into a glowing band.)
		colour += (min(glints, 1.0) * 0.4 * min(light, 1.2) + min(fresnel, TESR_BloodFlowLook.y) * surroundings * sheenTint * (1.0 - rough) * 0.5 * TESR_BloodFlowLook.x) * gloss;
	}
	// A falling drop: a dark red bead with a faint glint, in front of whatever is behind it (lit about as that is).
	float3 drop = light * (freshColour * 0.7 + glintTint * TESR_BloodFlowFresh.w);
	colour = lerp(colour, drop, p.air);
	// UNOFFICIAL 2026-10-13 (user: crushed bright blue where a muzzle flash lit the blood through smoke): this pass runs on the
	// HDR picture (before the tonemap), where a flash-lit floor is far above 1. Capped at 1, the colour fell below what the
	// blend below has to take away, its red and green went negative and were cut to 0: only blue was left. The ceiling is now
	// the brighter of 1 and the scene here.
	float ceiling = max(max(sceneLin.r, max(sceneLin.g, sceneLin.b)), 1.0);
	colour = min(colour, ceiling);

	// Blended over the image by `alpha`: the colour is given as what the pixel would be at full strength, so where the
	// image is still what this read, the result is exactly `colour`.
	float alpha = amount * IN.c.y;
	float3 full = (colour - sceneLin * (1.0 - amount)) / max(amount, 0.004);
	// (Scaled down as a whole past 1 rather than clipped channel by channel: a clipped red turned bright blood orange and yellow.)
	full = max(full, 0.0);
	full *= ceiling / max(max(full.r, max(full.g, full.b)), ceiling);
	return float4(delinearize(full), alpha);
}

// UNOFFICIAL 2026-10-13 (perf, user: pools close up cost a lot; F10: paint 1.3 ms for 96 items): BloodPS ran over the CPU's
// worst-case box round each item (a pool's reaches 4.6 times its radius every way) and only then dropped the pixels
// outside the item's real bounds. Here the box is cut down to those bounds (the bounds pass, drawn just before, plus a
// margin of a few pixels) before it is drawn, so those pixels never start. VSIN position: xyz the corner of the CPU's box,
// item-relative; w its face (0 -x, 1 +x, 2 -y, 3 +y, 4 -z, 5 +z: BloodFlowBoxes::Build). Of each pair of faces only the one
// past the eye is kept, as the CPU did before: every pixel the box covers is shaded once, the eye in it or not. Every
// vertex of a face works out the same plane (the margin is per item), so a face is kept or dropped whole.
VSOUT CutBox(VSIN IN, float limit) {
	VSOUT OUT;
	OUT.a = IN.a;
	OUT.b = IN.b;
	OUT.c = IN.c;
	OUT.d = IN.d;
	float3 corner = IN.position.xyz;
	float margin = 1.5 + 0.004 * length(IN.a.xyz);
	if (TESR_BloodFlowShape.w > 0.5) {
		float slot, pool, fresh;
		DecodeSlot(IN.a.w, slot, pool, fresh);
		float boundsV = (slot * Streams + Streams * 0.5) * TESR_BloodFlowProbe.x;	// as BloodPS
		float4 lo = tex2Dlod(BloodFlowBoundsVS, float4(0.25, boundsV, 0.0, 0.0));
		float4 hi = tex2Dlod(BloodFlowBoundsVS, float4(0.75, boundsV, 0.0, 0.0));
		// UNOFFICIAL 2026-10-20 (user + auto-test save nvrautotest14: pools cut on straight lines at this box's edges): pools keep
		// the CPU's box (their shape and the ground's slope can reach past the bounds; they cost ~2 ms for 23).
		if (lo.w > 0.5 && hi.w > 0.5) {
			// (2026-10-20 b, auto-test: pools drawn in the CPU's whole box, 4.6 radii every way, cost 2.1 -> 6.3 ms for 26.) A pool's
			// box: its paths' bounds joined with the room its shape can take (stretched and moved downhill: 3.1 radii; the ground
			// up to about 30 degrees round it), so it is never cut and never much bigger than it.
			float3 room = pool > 0.5 ? float3(4.0, 4.0, 3.2) * IN.b.w + float3(6.0, 6.0, 20.0) : float3(0.0, 0.0, 0.0);
			corner = clamp(corner, min(lo.xyz, -room) - margin, max(hi.xyz, room) + margin);
		}
	}
	corner = clamp(corner, -limit - margin, limit + margin);
	float3 rel = IN.a.xyz + corner;		// eye-relative
	float face = floor(IN.position.w + 0.5);
	float axis = floor(face * 0.5);
	float side = face - axis * 2.0 > 0.5 ? 1.0 : -1.0;
	float plane = axis < 0.5 ? rel.x : (axis < 1.5 ? rel.y : rel.z);
	// A dropped face: all its vertices behind the near plane, so nothing is drawn.
	OUT.position = side * plane > 0.0 ? mul(float4(rel + TESR_BloodFlowShape.xyz, 1.0), TESR_ViewProjectionTransform) : float4(0.0, 0.0, -2.0, 1.0);
	return OUT;
}

VSOUT PaintVS(VSIN IN) { return CutBox(IN, 1e9); }
// The wet beads lie on the game's splat only (BeadsPS): its box needs no more than the splat itself (as BoundsPS).
VSOUT BeadsVS(VSIN IN) {
	VSOUT OUT = CutBox(IN, IN.b.w * 1.1 + 1.0);
	float slot, pool, fresh;
	DecodeSlot(IN.a.w, slot, pool, fresh);
	if (pool > 0.5 || TESR_BloodFlowBeads.x <= 0.0) OUT.position = float4(0.0, 0.0, -2.0, 1.0);	// (as SplatVS: BeadsPS draws none)
	return OUT;
}

technique BloodFlow
{
	pass
	{
		VertexShader = compile vs_3_0 PaintVS();
		PixelShader = compile ps_3_0 BloodPS();
		AlphaBlendEnable = true;
		SrcBlend = SrcAlpha;
		DestBlend = InvSrcAlpha;
		BlendOp = Add;
		SeparateAlphaBlendEnable = false;
		AlphaTestEnable = false;
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = None;
		ScissorTestEnable = false;
		ColorWriteEnable = 7;
	}
}

// UNOFFICIAL 2026-10-16 (GroundFit): how much of a splat's blood a pixel keeps, by how far its surface lies off the splat's plane
// (normal n, through the centre): a thin slab, so blood never lands on something in front of or behind the splat. Soft (a fade
// of at least a few pixels), and as the surface here tilts away from the plane the slab widens with the distance along it (a
// surface at angle a to the plane lies |q| tan a off it at |q| from the centre; counted up to 60 degrees), so a splat running
// onto a slope or round a bend is not cut on a line. GroundFit 0: the hard cut as before, at base, and none where the surface
// faces the splat's way less than minFacing.
float SlabKeep(float3 pp, float3 n, float3 N, float base, float aaUnits, float minFacing) {
	float off = abs(dot(pp, n));
	float c = dot(N, n);
	[branch] if (TESR_BloodFlowGround.x > 0.5) {
		float along = length(pp - n * dot(pp, n));
		float tilt = sqrt(saturate(1.0 - c * c)) / max(c, 0.5);
		float fade = max(aaUnits * TESR_BloodFlowGround.y, 1.5);
		return saturate((base + along * tilt + fade - off) / fade) * saturate((c - minFacing + 0.15) / 0.25);
	}
	return off > base || c < minFacing ? 0.0 : 1.0;
}

// ---------------------------------------------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-13 (user: wet beads on the splats): over each of the game's own blood splats, glossy domed drops of blood
// (BloodDrops.dds), laid out on the splat exactly as the game laid its decal (VSIN d, c.z: BloodFlow.cpp DecalLayout), only
// where its splatter texture holds blood, drying away drop by drop. A pass of its own after the blood (registers): it adds
// the drops' highlights (the sun where it reaches, the lamps, a reflection at low angles) and darkens a little under the
// thick ones; blended as colour + image x alpha.
float4 BeadsPS(PSIN IN) : COLOR0 {
	float2 uv = (IN.vpos + 0.5) * TESR_ReciprocalResolution.xy;
	float slot, pool, fresh;
	DecodeSlot(IN.a.w, slot, pool, fresh);
	if (pool > 0.5 || TESR_BloodFlowBeads.x <= 0.0 || TESR_BloodFlowProbe.w < 0.5 || dot(IN.d.xyz, IN.d.xyz) < 1e-12) discard;
	float viewmodelDepth = tex2D(TESR_DepthBufferViewModel, uv).x;
	if ((TESR_DepthConstants.z == 0 && viewmodelDepth < 0.9) || (TESR_DepthConstants.z > 0 && viewmodelDepth > 0.01)) discard;
	float depth = readDepth(uv);
	if (depth >= farZ * 0.979) discard;
	float3 rel = toWorld(uv) * depth;
	float3 pp = rel - IN.a.xyz;
	float3 n = normalize(IN.b.xyz);
	float3 N = normalize(GetWorldNormalLod(uv));
	float slab = SlabKeep(pp, n, N, IN.b.w * 0.5 + 4.0, max(depth * TESR_ReciprocalResolution.y * 2.0 / TESR_ProjectionTransform[1][1], 0.05), -2.0);
	if (slab <= 0.0) discard;	// off the splat's surface (something in front, or behind)
	// The decal's own texture coordinates (DecalCover's layout), and where on the splatter atlas they are.
	float code = floor(floor(IN.c.z + 0.5) / 4096.0);
	float quarter = fmod(code, 5.0);
	float ratio = exp2(floor(code / 20.0) / 31.5 - 1.0);
	float3 vAxis = cross(n, IN.d.xyz) * ratio * (fmod(floor(code / 10.0), 2.0) > 0.5 ? -1.0 : 1.0);
	float2 local = float2(dot(pp, IN.d.xyz), dot(pp, vAxis)) + 0.5;
	if (any(local < 0.0) || any(local > 1.0)) discard;
	float2 atlas = quarter < 3.5 ? local * 0.5 + float2(fmod(quarter, 2.0), floor(quarter * 0.5)) * 0.5 : local;
	bool mirrored = fmod(floor(code / 5.0), 2.0) > 0.5;
	if (mirrored) atlas.x = 1.0 - atlas.x;
	float width = 1.0 / length(IN.d.xyz);
	float aa = max(depth * TESR_ReciprocalResolution.y * 2.0 / TESR_ProjectionTransform[1][1], 0.05);
	float blood = tex2Dlod(TESR_BloodSplatter, float4(atlas, 0.0, log2(max(aa / width * 1024.0, 1.0)))).a;
	if (blood < 0.25) discard;
	// The drops: tiled over the decal at about BeadSize units a tile; each shows while its own random value is under how wet
	// the splat still is, so they dry away one by one.
	float tiles = max(width / max(TESR_BloodFlowBeads.y, 1.0), 1.0);
	float4 drop = tex2Dlod(TESR_BloodDrops, float4(local * tiles, 0.0, log2(max(aa / width * tiles * 1024.0, 1.0))));
	float wet = 1.0 - smoothstep(0.0, TESR_BloodFlowDry.w * 0.8, IN.c.x);
	float show = saturate((wet * 1.1 - drop.a) * 6.0) * saturate(blood * 1.6 - 0.4) * slab;
	float h = drop.r * show;
	if (h < 0.01) discard;
	float2 slope = (drop.gb * 2.0 - 1.0) * show;
	// Its surface: the wall's normal tilted along the drop's slope (texture x along u; y up the texture image = against v).
	float3 T = normalize(IN.d.xyz) * (mirrored ? -1.0 : 1.0);
	float3 B = normalize(vAxis);
	float3 Nb = normalize(N + (T * slope.x - B * slope.y) * 1.5);
	float3 sceneLin = linearize(max(tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0, 0.0)).rgb, 0.0));
	float light = min(max(sceneLin.r, max(sceneLin.g, sceneLin.b)) / AssumedAlbedo, 2.5);
	float3 V = -normalize(rel);
	float a2 = 0.06 * 0.06 * 0.06 * 0.06;
	float glints = 0.0;
	if (TESR_BloodFlowInfo.x > 0.5) {
		float sunAmbient = max(luma(linearize(TESR_SunAmbient.rgb)), 0.001);
		float4 shadows = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0, 0.0));
		float lit = saturate(pow(abs((shadows.r + shadows.g) / sunAmbient), 5.0));
		glints += Glint(Nb, V, normalize(TESR_SunDirection.xyz), a2) * lit * saturate(luma(linearize(TESR_SunColor.rgb)));
	}
	float3 P = TESR_CameraPosition.xyz + rel;
	[loop] for (int i = 0; i < 12; i++) {
		if (TESR_ShadowLightPosition[i].w <= 0.0 && TESR_LightPosition[i].w <= 0.0) break;	// (the lamps are packed from the first slot)
		glints += LampGlint(Nb, V, P, TESR_ShadowLightPosition[i], TESR_LightColor[i].rgb, a2);
		glints += LampGlint(Nb, V, P, TESR_LightPosition[i], TESR_LightColor[i + 12].rgb, a2);
	}
	float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(Nb, V)), 5.0);
	float3 surroundings = TESR_BloodFlowInfo.x > 0.5 ? linearize(saturate(TESR_HorizonColor.rgb)) : light * 0.1;
	float strength = TESR_BloodFlowBeads.x * IN.c.y;
	// (UNOFFICIAL 2026-10-27 TESR_BloodFlowLook, as BloodPS: each bead's grey rim of mirrored sky made the splats look like heaps of
	// glass beads; the sheen is weaker, capped, and part in the hue of the splat under it.)
	float3 sheenTint = lerp(1.0, sceneLin / max(max(sceneLin.r, max(sceneLin.g, sceneLin.b)), 1e-4), TESR_BloodFlowLook.z);
	float3 shine = (min(glints, 1.0) * 0.45 * min(light, 1.2) + min(fresnel, TESR_BloodFlowLook.y) * surroundings * sheenTint * 0.4 * TESR_BloodFlowLook.x) * h * strength;
	// colour + image x alpha, in the image's own (display) encoding: the highlight as the step it makes over this pixel.
	float darken = 1.0 - 0.3 * h * saturate(strength);
	float3 under = sceneLin * darken;
	return float4(max(delinearize(under + shine) - delinearize(under), 0.0), darken);	// (HDR here: no cap at 1)
}

technique BloodBeads
{
	pass
	{
		VertexShader = compile vs_3_0 BeadsVS();
		PixelShader = compile ps_3_0 BeadsPS();
		AlphaBlendEnable = true;
		SrcBlend = One;
		DestBlend = SrcAlpha;
		BlendOp = Add;
		SeparateAlphaBlendEnable = false;
		AlphaTestEnable = false;
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = None;
		ScissorTestEnable = false;
		ColorWriteEnable = 7;
	}
}

technique BloodFlowBounds
{
	pass
	{
		VertexShader = compile vs_3_0 BoxVS();
		PixelShader = compile ps_3_0 BoundsPS();
		AlphaBlendEnable = false;
		AlphaTestEnable = false;
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = None;
		ScissorTestEnable = false;
		ColorWriteEnable = 15;
	}
}

technique BloodFlowProbe
{
	pass
	{
		VertexShader = compile vs_3_0 BoxVS();
		PixelShader = compile ps_3_0 ProbePS();
		AlphaBlendEnable = false;
		AlphaTestEnable = false;
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = None;
		ScissorTestEnable = false;
		ColorWriteEnable = 15;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// UNOFFICIAL 2026-10-14 (user: like paint thrown at a wall: arms and rays going out in random directions, never one solid glob,
// and no two throws alike): each splat's own impact shape, drawn over the game's decal (its texture has only four splatters).
// The same rules for every hit, the dice per hit (the item's seed):
//  - a core with a lumpy edge;
//  - 5 to 11 thick arms in random directions (a slanted hit bunches them to one side and stretches them), each its own length and
//    width, tapering, wobbling a little, some broken into a dotted line, ending in a droplet; one or two droplets flung on past
//    the tip, stretched along the way they flew;
//  - 20 to 60 hair-thin spray lines, fanned toward the throw's way, broken into dashes and dots at their ends;
//  - a fine mist of droplets round it, stretched outward;
//  - on a wall or a slope, a curtain of drips from the core's lower edge and a drip from each arm's tip, each its own length,
//    a bead at its end, growing over the first seconds (references: the user's photos of red paint thrown at walls);
//  - shot out over the first tenth of a second;
//  - on a wall or a slope the downhill arms and the core's lower edge sag on with time (the drips and tongues the streams draw
//    run on from there).
// A thickness field: the core and the arms' roots thick, the tips and the mist thin (thin dries first, as in BloodPS).
// The splat's thickness at q (in splat radii; +y down the surface; > 0 inside). grow 0-1: the impact; run 0-1: how far the drips
// have run (0 on the ground: nothing drips there).
// UNOFFICIAL 2026-10-22 (SplatMask): T read from the splat's cell of the mask atlas at q (in splat radii). code: 1 + cell x 1024 +
// the texels across it (BloodFlowBoxes::SplatMask, in VSIN c.w for the splat pass). Between texels it is blended (T is a smooth
// field there; a texel is about a pixel on screen).
float MaskT(float2 q, float code) {
	float c = floor(code + 0.5) - 1.0;
	float cell = floor(c / 1024.0);
	float n = c - cell * 1024.0;
	float E = TESR_BloodFlowMask.y;
	float2 local = float2((q.x + E) / (2.0 * E), (q.y + E) / (2.0 * E + TESR_BloodFlowMask.z));
	float row = floor(cell / TESR_BloodFlowMask.w);
	float2 corner = float2(cell - row * TESR_BloodFlowMask.w, row) * TESR_BloodFlowMaskSize.z;
	float2 texel = corner + clamp(local * n, 0.5, n - 0.5);
	return tex2Dlod(TESR_BloodSplatMask, float4(texel * TESR_BloodFlowMaskSize.xy, 0.0, 0.0)).x;
}

// UNOFFICIAL 2026-10-22 (perf, user: very hard fps drops; F10 'Blood splat shapes' 30-44 ms with 13-22 splats): fast (SplatFast,
// technique BloodSplatFast) gives the same picture with less work. Every part of the shape adds to T only as max(T, part), and a
// part at or below -0.15 can never show (SplatShade: cover is 0 below -0.15 whatever the rest is), so a part whose cheap bounds
// put the pixel too far from it for more than that is skipped: an arm (its direction is cheap, its root radius is not) unless the
// pixel lies near the ray it is on or in the strip its drip can hang in; a spray line's drips unless the pixel is above where they
// can reach; the drip curtain unless the pixel is near the core's lower edge. armDrip is then read from the path texture only for
// an arm that is near (slot: the item's slot). fast false: the old path, exactly as before (A/B).
float SplatThickness(float2 q, float seed, float grow, float run, float armDrip[6], bool fast, float slot) {
	float r = length(q);
	float a = atan2(q.y, q.x);
	float slant = 0.3 + 0.7 * hash1(seed * 3.71);	// a little glancing (nothing hits dead straight on) .. a glancing throw
	float lean = hash1(seed * 5.93) * 6.2832;
	float2 leanDir = float2(cos(lean), sin(lean));
	float coreR = SplatCoreRadius(a, seed, grow);
	float T = (coreR - r) / max(coreR, 0.05) * 1.6;
	float reach = TESR_BloodFlowSplat.y;
	// UNOFFICIAL 2026-10-14 (user: frames tank staring at blood up close, where one splat fills the screen): deep inside the core
	// nothing below can change the picture (the spray's thickness tops out at 0.9, an arm's adds little shading), so stop here.
	[branch] if (T >= 0.9) return T;

	// Thick arms: blobs thrown out of the core, tapering, ending in a droplet, a drip running down from it on a wall.
	float arms = SplatArms(seed);
	// (fast: no arm's root reaches past 0.7 x the core's largest radius, SplatCoreRadius's base x 1.38.)
	float rootMax = 0.966 * (0.45 + 0.18 * hash1(seed * 7.13)) * (0.6 + 0.4 * grow);
	[loop] for (float i = 0.0; i < 12.0; i += 1.0) {
		if (i >= arms) break;
		float h1 = hash1(seed * 13.7 + i * 7.31), h3 = hash1(seed * 41.9 + i * 5.53), h4 = hash1(seed * 57.1 + i * 2.29);
		[branch] if (fast) {
			// The arm's direction as SplatArm works it out; everything it draws lies within 1.69 x its width of the ray from the centre
			// out to its longest possible tip (its body: 1.12 widths + 0.02; its droplet: 1.69), or in the strip below that its drip can
			// hang down (1.0 widths either side of the drip and its bead).
			float h2 = hash1(seed * 29.3 + i * 3.17);
			float2 dir = float2(cos(h1 * 6.2832), sin(h1 * 6.2832));
			float2 bent = lerp(dir, leanDir, slant * 0.6 * h4);
			dir = dot(bent, bent) > 1e-4 ? normalize(bent) : dir;
			float width = 0.05 + 0.1 * h3;
			float far = max(rootMax, (rootMax + (0.3 + 0.9 * h2 * h2) * reach * (1.0 + dot(dir, leanDir) * slant * 0.7)) * grow) + width * 0.5;
			float2 tipMax = dir * far;
			float tq;
			bool nearArm = SegmentDistance(q, float2(0.0, 0.0), tipMax, tq) < 1.7 * width + 0.03;
			float dripMax = run * (0.15 + 0.9 * h4 * h4) * (0.6 + 0.6 * width / 0.15);
			float edge = width + 0.03;
			bool nearDrip = dripMax > 0.01 && q.x > min(0.0, tipMax.x) - edge && q.x < max(0.0, tipMax.x) + edge
				&& q.y > min(0.0, tipMax.y) - edge && q.y < max(0.0, tipMax.y) + dripMax + edge;
			if (!nearArm && !nearDrip) continue;
		}
		float2 dir;
		float rootR, len, width;
		SplatArm(i, seed, grow, dir, rootR, len, width);
		float t;
		float d = SegmentDistance(q, dir * rootR, dir * len, t);
		float w = width * (1.0 - 0.7 * t) + 0.02 * sin(t * 11.0 + h1 * 40.0);
		T = max(T, (w - d) / max(width, 0.02) * 1.3);
		float2 tip = dir * (len + width * 0.5);
		float dropR = width * (1.0 + 0.5 * h3);
		T = max(T, (dropR - length(q - tip)) / max(dropR, 0.02) * 1.2);
		// The drip down from the tip (walls): its own length, a bead at its end.
		// (Not when this arm has a real stream, which runs over the true surface instead: the probe's row for it, see ProbePS.)
		float dripLen = run * (0.15 + 0.9 * h4 * h4) * (0.6 + 0.6 * width / 0.15);
		[branch] if (fast) {
			[branch] if (dripLen > 0.01 && i < Streams) dripLen *= PathTexel(0.0, slot * Streams + i).w < -0.5 ? 1.0 : 0.0;
		}
		else dripLen *= i < Streams ? armDrip[(int)i] : 1.0;
		[branch] if (dripLen > 0.01) {
			float dw = dropR * 0.45;
			float dd = SegmentDistance(q, tip, tip + float2(0.0, dripLen), t);
			T = max(T, (dw * (1.0 - 0.25 * t) - dd) / max(dw, 0.01) * 1.1);
			T = max(T, (dw * 1.35 - length(q - tip - float2(0.0, dripLen))) / max(dw, 0.01) * 1.1);
		}
	}

	// Spray: hair-thin lines shot out far, fanned toward the throw's way, broken into dashes and dots toward their ends.
	float lines = 20.0 + floor(hash1(seed * 17.9) * 40.0 * TESR_BloodFlowSplat.z);
	// Not an even ring (user: they formed a circle): the lines fly out in a few bunches, most toward the throw's way, each line
	// starting at the lumpy core's own edge, a few long and many short; a few loners elsewhere stay short.
	float fan = lerp(2.4, 0.8, slant);
	float bunches = 2.0 + floor(hash1(seed * 23.3) * 3.0);
	[loop] for (float j = 0.0; j < 60.0; j += 1.0) {
		if (j >= lines) break;
		float g1 = hash1(seed * 61.3 + j * 3.91), g2 = hash1(seed * 73.7 + j * 1.77), g3 = hash1(seed * 89.1 + j * 6.07);
		float g4 = hash1(seed * 37.7 + j * 4.43);
		float b = floor(g4 * bunches);
		float bAng = lean + (hash1(seed * 45.1 + b * 7.7) - 0.5) * 2.0 * fan;
		float bSpread = 0.12 + 0.3 * hash1(seed * 19.9 + b * 3.3);
		float loner = step(0.82, g4 * 0.6 + g2 * 0.4);
		float ang = loner > 0.5 ? g1 * 6.2832 : bAng + (g1 - 0.5) * 2.0 * bSpread;
		float2 dir = float2(cos(ang), sin(ang));
		float toward = 0.5 + 0.5 * dot(dir, leanDir);
		// UNOFFICIAL 2026-10-14: a quick test first (the core radius below is costly): is the pixel anywhere near this line (along
		// it, within a hair of its direction) or, on a slope, below it where its drips hang? Most pixels are near only a few lines.
		float lenBound = (1.0 + (0.2 + 2.2 * g3 * g3) * reach * lerp(0.5, 1.3, toward) * (1.0 - 0.6 * loner)) * grow;
		float across = abs(dot(q, float2(-dir.y, dir.x))), along = dot(q, dir);
		bool nearLine = along > 0.0 && along < lenBound + 0.03 && across < 0.03;
		float2 tip = dir * lenBound;
		bool nearDrips = run > 0.0 && q.x > min(0.0, tip.x) - 0.03 && q.x < max(0.0, tip.x) + 0.03 && q.y > min(0.0, tip.y) - 0.03;
		// (fast: and not below where its drips can reach: at most run x reach x 0.63 under the line, the bead 0.033 more.)
		if (fast) nearDrips = nearDrips && q.y < max(0.0, tip.y) + run * reach * 0.63 + 0.04;
		[branch] if (!nearLine && !nearDrips) continue;
		float start = SplatCoreRadius(ang, seed, grow) * (0.75 + 0.35 * g2);
		float len = (start + (0.2 + 2.2 * g3 * g3) * reach * lerp(0.5, 1.3, toward) * (1.0 - 0.6 * loner)) * grow;
		float t;
		float d = SegmentDistance(q, dir * start, dir * len, t);
		float w = (0.008 + 0.014 * g2) * (1.0 - 0.6 * t);
		float dashes = t > 0.45 ? step(0.45, frac(t * (6.0 + 10.0 * g1) + g3)) : 1.0;
		T = max(T, dashes > 0.5 ? (w - d) / max(w, 0.004) * 0.9 : -1.0);	// (a gap: outside, not an edge)
		// On a wall or a slope a line's paint runs down too: one or two thin drips hang from points along it (user: the spray lines
		// flow with gravity, as in their photos).
		[branch] if (run > 0.0) {
			[unroll] for (int m = 0; m < 2; m++) {
				float g4 = hash1(seed * 101.3 + j * 2.53 + m * 7.1);
				[branch] if (g4 < 0.75 - 0.35 * m) {
					float2 from = dir * lerp(start, len, 0.2 + 0.7 * hash1(seed * 113.9 + j * 4.37 + m * 3.3));
					float dl = run * reach * (0.08 + 0.55 * g4 * g4);
					float dw = (0.008 + 0.014 * g2) * 0.9;
					float tt;
					float dd = SegmentDistance(q, from, from + float2(0.0, dl), tt);
					T = max(T, (dw * (1.0 - 0.3 * tt) - dd) / max(dw, 0.004) * 0.9);
					T = max(T, (dw * 1.5 - length(q - from - float2(0.0, dl))) / max(dw, 0.004) * 0.9);
				}
			}
		}
	}

	// The drip curtain: from the core's lower edge, side by side, each its own length (growing over the first seconds), a bead at
	// its end; the thickest near the middle of the edge.
	// (fast: the core's radius straight down worked out once, not per drip; and nothing here reaches more than 0.15 past it sideways
	// or below the lowest a drip can hang: its top at most 1.173 x the core's base radius, its length run x 2.4 x reach.)
	float coreDown = fast ? SplatCoreRadius(1.5708, seed, grow) : 0.0;
	bool curtain = !fast || (abs(q.x) < coreDown + 0.15 && q.y < 1.18 * (0.45 + 0.18 * hash1(seed * 7.13)) * (0.6 + 0.4 * grow) + run * 2.4 * reach + 0.15);
	[branch] if (run > 0.0 && q.y > 0.0 && curtain) {
		float cellW = 0.13;
		float cell = floor(q.x / cellW);
		[unroll] for (int k = -1; k <= 1; k++) {
			float c = cell + k;
			float e1 = hash1(seed * 97.3 + c * 1.37), e2 = hash1(seed * 31.1 + c * 2.71), e3 = hash1(seed * 53.9 + c * 0.93);
			float x = (c + 0.25 + 0.5 * e1) * cellW;
			float edge = 1.0 - saturate(abs(x) / max(fast ? coreDown : SplatCoreRadius(1.5708, seed, grow), 0.05));	// 1 under the middle, 0 at the sides
			[branch] if (e2 < 0.85 * sqrt(edge)) {
				float top = SplatCoreRadius(atan2(1.0, x), seed, grow) * sqrt(saturate(1.0 - x * x / 0.36)) * 0.85;
				float len = run * (0.2 + 2.2 * e3 * e3) * (0.4 + 0.6 * edge) * reach;
				float w = (0.025 + 0.045 * e2) * (0.6 + 0.6 * edge);
				float t;
				float dd = SegmentDistance(q, float2(x, top - 0.1), float2(x + 0.02 * (e1 - 0.5), top + len), t);
				T = max(T, (w * (1.0 - 0.2 * t) - dd) / max(w, 0.01) * 1.2);
				T = max(T, (w * 1.4 - length(q - float2(x, top + len))) / max(w, 0.01) * 1.2);
			}
		}
	}

	// A fine mist of droplets round it, stretched outward.
	[branch] if (TESR_BloodFlowSplat.z > 0.0 && r > coreR) {
		float bins = 48.0;
		float2 cell = float2(floor((a / 6.2832 + 0.5) * bins), floor(r * 7.0));
		float hc = hash2(cell + seed * 17.0);
		float side = 0.5 + 0.5 * dot(q / max(r, 1e-4), leanDir);	// thick toward the throw's way, thin behind
		[branch] if (hc < 0.25 * TESR_BloodFlowSplat.z * (0.25 + 1.1 * side * side) && r < (0.6 + 1.8 * reach * side) * grow) {
			float ca = ((cell.x + 0.2 + 0.6 * hash2(cell + 3.1)) / bins - 0.5) * 6.2832;
			float cr = (cell.y + 0.2 + 0.6 * hash2(cell + 7.7)) / 7.0;
			float2 c = float2(cos(ca), sin(ca)) * cr;
			float2 o = normalize(c);
			float2 rel = q - c;
			float2 e = float2(dot(rel, o) / 1.8, dot(rel, float2(-o.y, o.x)));
			float rm = 0.01 + 0.025 * hash2(cell + 11.3);
			T = max(T, (rm - length(e)) / rm * 0.8);
		}
	}
	return T;
}

VSOUT SplatVS(VSIN IN) {
	// Its own box (the arms reach past the bounds the streams set): the CPU's box, cut to the arms' reach.
	VSOUT OUT;
	OUT.a = IN.a; OUT.b = IN.b; OUT.c = IN.c; OUT.d = IN.d;
	float limit = ShapeR(IN.b.w) * (1.2 + 2.4 * TESR_BloodFlowSplat.y + 2.4 * TESR_BloodFlowSplat.w) + 2.0;
	float3 corner = clamp(IN.position.xyz, -limit, limit);
	float3 rel = IN.a.xyz + corner;
	float face = floor(IN.position.w + 0.5);
	float axis = floor(face * 0.5);
	float side = face - axis * 2.0 > 0.5 ? 1.0 : -1.0;
	float plane = axis < 0.5 ? rel.x : (axis < 1.5 ? rel.y : rel.z);
	OUT.position = side * plane > 0.0 ? mul(float4(rel + TESR_BloodFlowShape.xyz, 1.0), TESR_ViewProjectionTransform) : float4(0.0, 0.0, -2.0, 1.0);
	// UNOFFICIAL 2026-10-20 (perf, user: frames drop very hard with a little blood on screen; log: splat shapes 38 ms with ~20
	// pools): SplatPS draws nothing for a pool, but its 'discard' does not stop the shader, so every pixel of each pool's big
	// box ran the whole splat shape. A pool's box is dropped here, before any pixel starts.
	float slot, pool, fresh;
	DecodeSlot(IN.a.w, slot, pool, fresh);
	if (pool > 0.5 || TESR_BloodFlowSplat.x <= 0.0) OUT.position = float4(0.0, 0.0, -2.0, 1.0);
	return OUT;
}

// UNOFFICIAL 2026-10-22 (SplatMask; DeepSeek-V4-Pro's review: draw tight boxes, not world-axis cuboids): the splat's box in its own
// plane instead of the world's axes: across it and down it only as far as the shape reaches (SplatMaskPS's reach test, with the
// room its 2-pixel margin needs at the box's nearest point), the drips' room below; along the normal as far as SplatVS's cube
// reaches that way. Seen face on that is under half the cube's area on screen. The CPU's box (BloodFlowBoxes::Build) gives each
// vertex its corner (which side of each axis: the sign) and its face (w), as for CutBox.
VSOUT SplatTightVS(VSIN IN) {
	VSOUT OUT;
	OUT.a = IN.a; OUT.b = IN.b; OUT.c = IN.c; OUT.d = IN.d;
	float R = max(ShapeR(IN.b.w), 1.0);
	float3 n = normalize(IN.b.xyz);
	float3 down = float3(n.x * n.z, n.y * n.z, n.z * n.z - 1.0);
	float steep = length(down);
	down = steep > 0.05 ? down / steep : float3(1.0, 0.0, 0.0);
	float3 across = normalize(cross(n, down));
	float run = TESR_BloodFlowSplat.w * saturate(steep * 3.0 - 0.15) * (1.0 - exp(-max(IN.c.x - 0.3, 0.0) / 4.0));
	float dripMax = run > 0.0 ? run * max(1.3, 2.4 * TESR_BloodFlowSplat.y) + 0.2 : 0.0;
	float E = 1.15 + 3.12 * TESR_BloodFlowSplat.y;
	float limit = ShapeR(IN.b.w) * (1.2 + 2.4 * TESR_BloodFlowSplat.y + 2.4 * TESR_BloodFlowSplat.w) + 2.0;
	// The PS's margin (2 pixels' worth of aa) where it is widest, at the box's farthest point, plus a unit.
	float farthest = length(IN.a.xyz) + limit * 1.8;
	float margin = 2.0 * max(farthest * TESR_ReciprocalResolution.y * 2.0 / TESR_ProjectionTransform[1][1], 0.05) + 1.0;
	float depthRoom = limit * (abs(n.x) + abs(n.y) + abs(n.z));
	float3 lo = float3(-E * R - margin, -E * R - margin, -depthRoom);
	float3 hi = float3(E * R + margin, (E + dripMax) * R + margin, depthRoom);
	float3 local = IN.position.xyz > 0.0 ? hi : lo;
	float3 rel = IN.a.xyz + across * local.x + down * local.y + n * local.z;
	float face = floor(IN.position.w + 0.5);
	float axis = floor(face * 0.5);
	float side = face - axis * 2.0 > 0.5 ? 1.0 : -1.0;
	float3 outward = axis < 0.5 ? across : (axis < 1.5 ? down : n);
	OUT.position = side * dot(rel, outward) > 0.0 ? mul(float4(rel + TESR_BloodFlowShape.xyz, 1.0), TESR_ViewProjectionTransform) : float4(0.0, 0.0, -2.0, 1.0);
	float slot, pool, fresh;
	DecodeSlot(IN.a.w, slot, pool, fresh);
	if (pool > 0.5 || TESR_BloodFlowSplat.x <= 0.0) OUT.position = float4(0.0, 0.0, -2.0, 1.0);
	return OUT;
}

// UNOFFICIAL 2026-10-20 (perf: splat shapes 38 ms, user debug shot: most of each box cut away): the shape and look, in a
// function SplatPS calls only inside a real branch. ps_3_0 'discard' does not stop a pixel, so every cut-away or out-of-reach
// pixel used to run all of it.
float4 SplatShade(PSIN IN, float2 uv, float slot, float3 rel, float3 pp, float R, float3 N, float keep, bool cutView,
		float3 down, float3 across, float2 q, float seed, float age, float grow, float run, float aa, bool fast, float maskCode) {
	// The arms with a real stream (the probe found one: its row's end code is not 'none') draw no drip of their own.
	// (fast: read in SplatThickness, only for the arms near this pixel.)
	float armDrip[6];
	[unroll] for (int ai = 0; ai < 6; ai++) armDrip[ai] = fast ? 1.0 : (PathTexel(0.0, slot * Streams + ai).w < -0.5 ? 1.0 : 0.0);
	float T;
	// (SplatMask: from the splat's cell when it has one; 2026-10-22.)
	[branch] if (maskCode > 0.5) T = MaskT(q, maskCode);
	else T = SplatThickness(q, seed, grow, run, armDrip, fast, slot);
	float cover = saturate(T / max(aa * 2.0, 0.02) + 0.5) * saturate(T * 4.0 + 0.6);
	[branch] if (cutView) {
		// UNOFFICIAL 2026-10-16 debug view 2 (the cuts): where the splat's shape has blood, red above the top of the box it had
		// before GroundFit (its centre + radius + 6 units: arms and spray thrown up a wall were cut flat there), magenta where
		// the slab takes it away, orange where it only thins it, yellow where it shows.
		if (cover < 0.5) discard;
		if (TESR_BloodFlowGround.x < 0.5 && pp.z > R + 6.0) return float4(1, 0, 0, 0.9);
		return keep < 0.05 ? float4(1, 0, 1, 0.9) : keep < 0.95 ? float4(1, 0.5, 0, 0.9) : float4(1, 0.9, 0, 0.85);
	}
	cover *= keep;
	if (cover < 0.004) discard;
	float thick = saturate(T * 0.6);

	// The look, as BloodPS's: the light here, the blood's colour (the game's decal colour when matched), drying thin first.
	float3 sceneLin = linearize(max(tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0, 0.0)).rgb, 0.0));
	float lit = 0.0;
	if (TESR_BloodFlowInfo.x > 0.5) {
		float sunAmbient = max(luma(linearize(TESR_SunAmbient.rgb)), 0.001);
		float4 shadows = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0, 0.0));
		lit = saturate(pow(abs((shadows.r + shadows.g) / sunAmbient), 5.0));
	}
	float irradiance = luma(linearize(saturate(TESR_SunAmbient.rgb))) + lit * luma(linearize(saturate(TESR_SunColor.rgb))) * saturate(dot(N, normalize(TESR_SunDirection.xyz)));
	float light = min(max(max(sceneLin.r, max(sceneLin.g, sceneLin.b)) / AssumedAlbedo, irradiance), 2.5);
	float dry = smoothstep(0.0, 1.0, age / (TESR_BloodFlowDry.w * lerp(0.5, 1.6, thick)));
	float3 freshColour = TESR_BloodFlowFresh.rgb;
	float3 dryColour = TESR_BloodFlowDry.rgb;
	[branch] if (TESR_BloodFlowMatch.w > 0.5 && IN.d.w > 0.5) {
		float tc = floor(IN.d.w + 0.5);
		float cr = floor(tc / 65536.0);
		cr += (tc - cr * 65536.0 >= 65536.0) ? 1.0 : ((tc - cr * 65536.0 < 0.0) ? -1.0 : 0.0);
		float gb = tc - cr * 65536.0;
		float cg = floor(gb / 256.0);
		cg += (gb - cg * 256.0 >= 256.0) ? 1.0 : ((gb - cg * 256.0 < 0.0) ? -1.0 : 0.0);
		freshColour = linearize(saturate(float3(cr, cg, gb - cg * 256.0) / 255.0 * TESR_BloodFlowMatch.x)) * TESR_BloodFlowMatch.y;
		freshColour *= max(1.0, TESR_BloodFlowLook.w / max(max(freshColour.r, max(freshColour.g, freshColour.b)), 1e-4));	// (2026-10-27 MatchFloor, as BloodPS)
		dryColour = freshColour * float3(0.72, 0.8, 0.8) + freshColour.r * float3(0.0, 0.1, 0.05);
	}
	float3 albedo = lerp(freshColour, dryColour, dry) * lerp(1.0, 0.8 + 0.4 * valueNoise(q * R * 0.35 + seed * 31.0), dry);
	float3 through = exp(-(1.2 + log(max(max(albedo.r, max(albedo.g, albedo.b)), 0.001) / max(albedo, 0.001))) * (0.25 + 2.0 * thick));
	float3 colour = sceneLin * through + light * albedo * lerp(1.0, 0.6, saturate(thick * 1.3 - 0.3)) * (1.0 - through);
	// Wet and glossy while fresh: the sun's and the lamps' highlights, a reflection at low angles, on a surface rounded at the
	// splat's edges (the thickness falling off).
	float gloss = TESR_BloodFlowFresh.w * (1.0 - dry) * lerp(0.5, 1.0, thick);
	[branch] if (gloss > 0.001) {
		float3 V = -normalize(rel);
		float3 Nb = normalize(N + (across * q.x + down * q.y) * saturate(1.0 - T) * 0.15);	// (rounded toward the edges)
		float a2 = pow(lerp(0.1, 0.4, dry), 4.0);
		float glints = 0.0;
		if (TESR_BloodFlowInfo.x > 0.5) glints += Glint(Nb, V, normalize(TESR_SunDirection.xyz), a2) * lit * saturate(luma(linearize(TESR_SunColor.rgb)));
		float3 P = TESR_CameraPosition.xyz + rel;
		[loop] for (int i = 0; i < 12; i++) {
			if (TESR_ShadowLightPosition[i].w <= 0.0 && TESR_LightPosition[i].w <= 0.0) break;
			glints += LampGlint(Nb, V, P, TESR_ShadowLightPosition[i], TESR_LightColor[i].rgb, a2);
			glints += LampGlint(Nb, V, P, TESR_LightPosition[i], TESR_LightColor[i + 12].rgb, a2);
		}
		float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(Nb, V)), 5.0);
		float3 surroundings = TESR_BloodFlowInfo.x > 0.5 ? linearize(saturate(TESR_HorizonColor.rgb)) : light * 0.1;
		// (2026-10-27 TESR_BloodFlowLook, as BloodPS: the sheen weaker, capped, part in the blood's hue.)
		float3 sheenTint = lerp(1.0, albedo / max(max(albedo.r, max(albedo.g, albedo.b)), 1e-4), TESR_BloodFlowLook.z);
		colour += (min(glints, 1.0) * 0.4 * min(light, 1.2) + min(fresnel, TESR_BloodFlowLook.y) * surroundings * sheenTint * 0.4 * TESR_BloodFlowLook.x) * gloss;
	}
	float ceiling = max(max(sceneLin.r, max(sceneLin.g, sceneLin.b)), 1.0);
	colour = min(colour, ceiling);
	float alpha = cover * IN.c.y * saturate(TESR_BloodFlowSplat.x);
	return float4(delinearize(max(colour, 0.0)), alpha);
}

float4 SplatBody(PSIN IN, bool fast, bool mask) {
	float2 uv = (IN.vpos + 0.5) * TESR_ReciprocalResolution.xy;
	float slot, pool, fresh;
	DecodeSlot(IN.a.w, slot, pool, fresh);
	if (pool > 0.5 || TESR_BloodFlowSplat.x <= 0.0) discard;
	float viewmodelDepth = tex2D(TESR_DepthBufferViewModel, uv).x;
	if ((TESR_DepthConstants.z == 0 && viewmodelDepth < 0.9) || (TESR_DepthConstants.z > 0 && viewmodelDepth > 0.01)) discard;
	float depth = readDepth(uv);
	if (depth >= farZ * 0.979) discard;
	float3 rel = toWorld(uv) * depth;
	float3 pp = rel - IN.a.xyz;
	float R = max(ShapeR(IN.b.w), 1.0);
	float3 n = normalize(IN.b.xyz);
	float3 N = normalize(GetWorldNormalLod(uv));
	// UNOFFICIAL 2026-10-20 (user: half-disc 'pools' cut on a straight line next to ragdolls): the game's decal plane is often the
	// ragdoll's, not the ground's, so a slab round it cut the splat in half. The surface here judged by its own plane instead: blood
	// shows where the surface's tangent plane passes near the splat's centre (the ground under it, not a body above it).
	// (2026-10-20 again, user: blood flickers in and out with the view angle: the pixel's own normal is too noisy a plane.) Ground
	// is judged by height over the splat's centre (more room on a slope); a wall by the game's plane as before.
	float3 slabN = N.z > 0.7 ? float3(0.0, 0.0, 1.0) : n;
	float keep = SlabKeep(pp, slabN, N, R * 0.6 + 4.0, max(depth * TESR_ReciprocalResolution.y * 2.0 / TESR_ProjectionTransform[1][1], 0.05), 0.3);
	bool cutView = TESR_BloodFlowInfo.y > 1.5;
	bool off = keep <= 0.0 && !cutView;	// off the splat's surface
	// The splat's plane: down the surface (+y), across it (+x).
	float3 down = float3(n.x * n.z, n.y * n.z, n.z * n.z - 1.0);
	float steep = length(down);
	down = steep > 0.05 ? down / steep : float3(1.0, 0.0, 0.0);
	float3 across = normalize(cross(n, down));
	float2 q = float2(dot(pp, across), dot(pp, down)) / R;
	float seed = ItemSeed(IN.c.z);
	float age = IN.c.x;
	float grow = smoothstep(0.0, 1.0, saturate(age / 0.12));
	// The drips run on a wall or a slope (none on level ground), most of the way in the first few seconds.
	float run = TESR_BloodFlowSplat.w * saturate(steep * 3.0 - 0.15) * (1.0 - exp(-max(age - 0.3, 0.0) / 4.0));	// (from about 3 degrees; full by about 20)
	float aa = max(depth * TESR_ReciprocalResolution.y * 2.0 / TESR_ProjectionTransform[1][1], 0.05) / R;
	// UNOFFICIAL 2026-10-16 (cheaper, the picture unchanged): nothing of the shape reaches past 0.96 + 3.12 x SplatReach radii from
	// the centre (the longest spray line: the core's edge, at most 0.96, plus 2.4 x 1.3 x reach; the arms, their droplets and the
	// mist stop sooner) but the drips, which hang below by at most run x max(1.26, 2.4 x reach) (an arm's tip drip; the curtain)
	// and a bead. Pixels farther out -- most of a splat's box seen at an angle -- skip the arm and spray loops.
	float reachMax = 1.15 + 3.12 * TESR_BloodFlowSplat.y + 2.0 * aa;
	float dripMax = run > 0.0 ? run * max(1.3, 2.4 * TESR_BloodFlowSplat.y) + 0.2 : 0.0;
	float2 qq = float2(q.x, q.y > 0.0 ? max(q.y - dripMax, 0.0) : q.y);
	[branch] if (off || dot(qq, qq) > reachMax * reachMax) discard;
	else return SplatShade(IN, uv, slot, rel, pp, R, N, keep, cutView, down, across, q, seed, age, grow, run, aa, fast, mask ? IN.c.w : 0.0);
	return float4(0.0, 0.0, 0.0, 0.0);
}

float4 SplatPS(PSIN IN) : COLOR0 { return SplatBody(IN, false, false); }
// UNOFFICIAL 2026-10-22: the same picture, cheaper (SplatThickness, fast; BloodFlow.cpp SplatFast).
float4 SplatFastPS(PSIN IN) : COLOR0 { return SplatBody(IN, true, false); }
// UNOFFICIAL 2026-10-22 (SplatMask): the shape read from the mask atlas (splats without a cell: as SplatFastPS), in the tight box
// SplatTightVS draws. As SplatBody, but the cheap test whether the pixel is in the splat's reach comes before the surface normal is
// read and the slab worked out (most pixels of the box are out of reach), and a pixel whose surface lies outside the box SplatVS
// drew (the old cube, `limit` round the centre) is dropped, as that box never reached it.
float4 SplatMaskPS(PSIN IN) : COLOR0 {
	float2 uv = (IN.vpos + 0.5) * TESR_ReciprocalResolution.xy;
	float slot, pool, fresh;
	DecodeSlot(IN.a.w, slot, pool, fresh);
	if (pool > 0.5 || TESR_BloodFlowSplat.x <= 0.0) discard;
	float viewmodelDepth = tex2D(TESR_DepthBufferViewModel, uv).x;
	if ((TESR_DepthConstants.z == 0 && viewmodelDepth < 0.9) || (TESR_DepthConstants.z > 0 && viewmodelDepth > 0.01)) discard;
	float depth = readDepth(uv);
	if (depth >= farZ * 0.979) discard;
	float3 rel = toWorld(uv) * depth;
	float3 pp = rel - IN.a.xyz;
	float R = max(ShapeR(IN.b.w), 1.0);
	float3 n = normalize(IN.b.xyz);
	float3 down = float3(n.x * n.z, n.y * n.z, n.z * n.z - 1.0);
	float steep = length(down);
	down = steep > 0.05 ? down / steep : float3(1.0, 0.0, 0.0);
	float3 across = normalize(cross(n, down));
	float2 q = float2(dot(pp, across), dot(pp, down)) / R;
	float age = IN.c.x;
	float run = TESR_BloodFlowSplat.w * saturate(steep * 3.0 - 0.15) * (1.0 - exp(-max(age - 0.3, 0.0) / 4.0));
	float aa = max(depth * TESR_ReciprocalResolution.y * 2.0 / TESR_ProjectionTransform[1][1], 0.05) / R;
	float reachMax = 1.15 + 3.12 * TESR_BloodFlowSplat.y + 2.0 * aa;
	float dripMax = run > 0.0 ? run * max(1.3, 2.4 * TESR_BloodFlowSplat.y) + 0.2 : 0.0;
	float2 qq = float2(q.x, q.y > 0.0 ? max(q.y - dripMax, 0.0) : q.y);
	float limit = ShapeR(IN.b.w) * (1.2 + 2.4 * TESR_BloodFlowSplat.y + 2.4 * TESR_BloodFlowSplat.w) + 2.0;	// (SplatVS's cube)
	[branch] if (dot(qq, qq) > reachMax * reachMax || any(abs(pp) > limit + 1.0)) discard;
	else {
		float3 N = normalize(GetWorldNormalLod(uv));
		float3 slabN = N.z > 0.7 ? float3(0.0, 0.0, 1.0) : n;	// (as SplatBody)
		float keep = SlabKeep(pp, slabN, N, R * 0.6 + 4.0, aa * R, 0.3);
		bool cutView = TESR_BloodFlowInfo.y > 1.5;
		[branch] if (keep <= 0.0 && !cutView) discard;
		else return SplatShade(IN, uv, slot, rel, pp, R, N, keep, cutView, down, across, q, ItemSeed(IN.c.z), age,
			smoothstep(0.0, 1.0, saturate(age / 0.12)), run, aa, true, IN.c.w);
	}
	return float4(0.0, 0.0, 0.0, 0.0);
}

// UNOFFICIAL 2026-10-22 (SplatMask): one cell of the mask atlas: T at each texel's point of the splat's square (BloodFlowBoxes::SplatMask:
// VSIN a.xy the cell's corner in texels, a.z its texels across; the rest as the splat pass), grown and run as SplatPS works it out.
float4 MaskBakePS(PSIN IN) : COLOR0 {
	float slot, pool, fresh;
	DecodeSlot(IN.a.w, slot, pool, fresh);
	float2 local = (IN.vpos - IN.a.xy + 0.5) / IN.a.z;
	float E = TESR_BloodFlowMask.y;
	float2 q = float2(local.x * 2.0 * E - E, local.y * (2.0 * E + TESR_BloodFlowMask.z) - E);
	float3 n = normalize(IN.b.xyz);
	float3 down = float3(n.x * n.z, n.y * n.z, n.z * n.z - 1.0);
	float steep = length(down);
	float age = IN.c.x;
	float grow = smoothstep(0.0, 1.0, saturate(age / 0.12));
	float run = TESR_BloodFlowSplat.w * saturate(steep * 3.0 - 0.15) * (1.0 - exp(-max(age - 0.3, 0.0) / 4.0));	// (as SplatPS)
	float armDrip[6];
	[unroll] for (int ai = 0; ai < 6; ai++) armDrip[ai] = 1.0;	// (fast: read inside)
	return float4(SplatThickness(q, ItemSeed(IN.c.z), grow, run, armDrip, true, slot), 0.0, 0.0, 1.0);
}

technique BloodSplat
{
	pass
	{
		VertexShader = compile vs_3_0 SplatVS();
		PixelShader = compile ps_3_0 SplatPS();
		AlphaBlendEnable = true;
		SrcBlend = SrcAlpha;
		DestBlend = InvSrcAlpha;
		BlendOp = Add;
		SeparateAlphaBlendEnable = false;
		AlphaTestEnable = false;
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = None;
		ScissorTestEnable = false;
		ColorWriteEnable = 7;
	}
}

// UNOFFICIAL 2026-10-22 (perf): the splat shapes, the same picture with less work per pixel (Shaders.BloodFlow.Main SplatFast).
technique BloodSplatFast
{
	pass
	{
		VertexShader = compile vs_3_0 SplatVS();
		PixelShader = compile ps_3_0 SplatFastPS();
		AlphaBlendEnable = true;
		SrcBlend = SrcAlpha;
		DestBlend = InvSrcAlpha;
		BlendOp = Add;
		SeparateAlphaBlendEnable = false;
		AlphaTestEnable = false;
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = None;
		ScissorTestEnable = false;
		ColorWriteEnable = 7;
	}
}

// UNOFFICIAL 2026-10-22 (SplatMask): the splat shapes read from the mask atlas (Shaders.BloodFlow.Main SplatMask).
technique BloodSplatMask
{
	pass
	{
		VertexShader = compile vs_3_0 SplatTightVS();
		PixelShader = compile ps_3_0 SplatMaskPS();
		AlphaBlendEnable = true;
		SrcBlend = SrcAlpha;
		DestBlend = InvSrcAlpha;
		BlendOp = Add;
		SeparateAlphaBlendEnable = false;
		AlphaTestEnable = false;
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = None;
		ScissorTestEnable = false;
		ColorWriteEnable = 7;
	}
}

// UNOFFICIAL 2026-10-22 (SplatMask): the cells of the mask atlas that need working out (one quad each, BloodFlowBoxes::SplatMask).
technique BloodSplatMaskBake
{
	pass
	{
		VertexShader = compile vs_3_0 BoxVS();
		PixelShader = compile ps_3_0 MaskBakePS();
		AlphaBlendEnable = false;
		AlphaTestEnable = false;
		ZEnable = false;
		ZWriteEnable = false;
		StencilEnable = false;
		CullMode = None;
		ScissorTestEnable = false;
		ColorWriteEnable = 15;
	}
}

// UNOFFICIAL 2026-11-12 Surface ([Shaders.BloodFlow.Surface], off by default): blood as a layer of liquid lying on the ground, simulated
// per splat and per body in a small patch of the world (techniques BloodSurfaceSim, BloodSurface). Last: its samplers take the next stages.
#include "Includes/BloodSurface.hlsl"
