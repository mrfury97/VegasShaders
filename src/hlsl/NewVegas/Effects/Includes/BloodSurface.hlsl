// UNOFFICIAL 2026-11-12 BloodFlow Surface ([Shaders.BloodFlow.Surface], off by default; src/effects/BloodFlow/BloodSurface.h).
// Included at the end of BloodFlow.fx.hlsl (it uses that file's helpers, constants and samplers).
// Blood as a thin layer of liquid lying ON the ground or wall, instead of shapes painted into the picture:
//  - Each fresh splat and each bleeding body gets a patch: a square fixed in the world (level for floors, upright for walls)
//    with a cell of a state atlas (TESR_BloodSurface, fp16, two copies). A texel holds r the blood's thickness there (game
//    units), g how dry it is (0 wet, 1 dry), b the real surface's height above the patch's plane, a what is known of that
//    height (0 nothing, 0.25 something tall stands there, 0.5 guessed from its neighbours, 1-8 read that many times).
//  - BloodSurfaceSim (SurfaceSimPS), a step of the simulation for every patch that is still wet, from one copy of the atlas
//    into the other (two steps a frame, so the first copy always holds the newest state):
//      heights: while the patch is on screen each texel finds its surface point in the depth buffer (walking along the
//        patch's normal) and keeps it, averaged over the first reads; what the camera cannot see (behind a rock, under the
//        body) is filled in smoothly from the texels round it. Actors, plants and the first-person weapon are never read
//        (TESR_MaterialBuffer's classes), so a body lying in its own pool is not part of the ground.
//      flow: blood moves to the lower of two neighbouring texels, lower counting the ground's height plus the blood on it
//        (and, on a slope or a wall, how far down the patch the texel is): faster the thicker it is (thickness cubed, as a
//        thin viscous film), never more than a share of what the texel holds, never past level. A thin film stays behind
//        wherever blood has been (the stain), dry ground holds the front back until the blood behind it stands high enough
//        (so the edge creeps on in lobes, not as a ring), blood dries (thin films first) and stiffens as it does, and soaks
//        away slowly. So it runs into cracks, parts round stones, gathers in hollows, and its outline is the ground's.
//      sources: a splat lays its blood down in the first half second, in the shape of the game's own decal (its texture's
//        alpha, where that texture is a loose file; else a drawn blob); a body bleeds for BleedTime under its middle.
//    When a patch's time is up it is frozen: no more steps, only the drawing below.
//  - BloodSurface (SurfacePS): a box round each patch; a pixel in it takes its surface point from the depth buffer, finds
//    its texel and shows the blood there only if that point lies on the height the patch holds (so nothing standing in or
//    passing through the patch is painted) and is ground or a rigid object (never an actor, a plant, the weapon in hand).
//    The colour is the pixel's own, relit: the light that is already there (the picture over the surface's own colour,
//    TESR_AlbedoBuffer; a fixed grey where there is no record) times what a film of blood of that thickness lets through
//    and scatters back. In shadow it is dark, in the sun lit, under a lamp the lamp's colour. Wet blood adds a highlight
//    of the sun (where it is not shaded) and the lamps, and a little of the sky at low angles; dry blood none.
// Limits: a patch is one flat sheet (blood running down a wall stops at its foot, it does not carry on over the floor);
// heights are read only while the patch is on screen; ground finer than a texel (about a game unit) is not seen.
#ifndef NVR_BLOODSURFACE_HLSL
#define NVR_BLOODSURFACE_HLSL

// (Declared in register order after BloodFlow.fx.hlsl's s0-s12: the samplers bind by the order they are declared in.)
sampler2D TESR_BloodSurface : register(s13) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = NONE; };
sampler2D TESR_AlbedoBuffer : register(s14) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_MaterialBuffer : register(s15) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
float4 TESR_AlbedoBufferData;		// x: 1 the albedo buffer holds this frame's (core/CapturedNormals.h)
float4 TESR_MaterialBufferData;		// x: 1 the material buffer holds this frame's
float4 TESR_BloodSurfaceSize;		// xy: 1 / the atlas's size in texels; z: a cell's size in texels; w: cells per atlas row
float4 TESR_BloodSurfaceSim;		// x: seconds this step; y: 1 this step reads the depth buffer; z: how easily blood moves (1 / viscosity);
									// w: the ground's fine roughness (game units)
float4 TESR_BloodSurfaceFlow;		// x: a splat's thickness where its decal is solid (game units); y: a pool's volume over its radius squared;
									// z: seconds a body bleeds; w: thickness that soaks away a second
float4 TESR_BloodSurfaceLook;		// x: thickness at which blood hides what it lies on; y: how much higher blood must stand to wet dry ground;
									// z: debug view; w: 1 the light is worked out from the albedo buffer where it has a record

static const float SurfRes = 0.02;		// the film left behind where blood has been (game units)
static const float SurfDryCell = 0.004;	// thinner than this: dry ground
static const float SurfTall = 25.0;		// something that stands this far out of the ground and hides it: blood does not go there
// The blood colours in the settings were set by eye against a light worked out as the picture over AssumedAlbedo (0.3). This
// game's surfaces record far darker colours than that (sand about 0.06, a plank floor about 0.08), so the light worked out from
// the record is some four times stronger, and the same colours came out as bright as ketchup (in game, 2026-11-12: with the
// record bright orange-red on sand, without it a deep red). With the record the blood's own colour is taken down by this over
// AssumedAlbedo: on a typical surface it then looks as it does without the record, on pale ones darker than the surface, on
// black ones still red. It also brings a matched colour (MatchFloor's 0.2) back to about what the game's own decal has.
static const float SurfTypicalAlbedo = 0.07;

struct SURFV {
	float4 position : POSITION;
	float4 a : TEXCOORD0;		// xyz: the patch's centre, eye-relative; w: its cell + 64 x (1 a body's pool + 2 its cell is new + 4 on screen)
	float4 b : TEXCOORD1;		// xyz: the patch's normal; w: half its side (game units)
	float4 c : TEXCOORD2;		// x: seconds simulated, this step's (steps) / seconds since it froze (drawing); y: how freely it still moves (steps) /
								// fade (drawing); z: seed + 4096 x the decal's layout code (as VSIN); w: the splat's or pool's radius
	float4 d : TEXCOORD3;		// as VSIN: xyz the decal texture's u axis over its width; w its colour
	float4 u : TEXCOORD4;		// xyz: the patch's first axis; w: where the splat or body is on it, from the centre (steps)
	float4 v : TEXCOORD5;		// xyz: its second axis; w: likewise
};

struct SURFP {
	float4 a : TEXCOORD0;
	float4 b : TEXCOORD1;
	float4 c : TEXCOORD2;
	float4 d : TEXCOORD3;
	float4 u : TEXCOORD4;
	float4 v : TEXCOORD5;
	float2 vpos : VPOS;
};

// The patch's two axes from its normal (BloodSurface.h Axes: the same sums).
void SurfAxes(float3 n, out float3 U, out float3 V) {
	float3 ref = abs(n.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
	U = normalize(cross(ref, n));
	V = cross(n, U);
}

void SurfCode(float w, out float2 origin, out float pool, out float reset, out float seen) {
	float code = floor(w + 0.5);
	float cell = fmod(code, 64.0);
	float flags = floor(code / 64.0);
	pool = fmod(flags, 2.0);
	reset = fmod(floor(flags * 0.5), 2.0);
	seen = fmod(floor(flags * 0.25), 2.0);
	origin = float2(fmod(cell, TESR_BloodSurfaceSize.w), floor(cell / TESR_BloodSurfaceSize.w)) * TESR_BloodSurfaceSize.z;
}

// A step's quad over the patch's cell (BloodSurface.h BuildSim: position xy in clip space, zw where the item is on the patch).
SURFV SurfaceSimVS(VSIN IN) {
	SURFV OUT;
	OUT.position = float4(IN.position.xy, 0.5, 1.0);
	OUT.a = IN.a;
	OUT.b = IN.b;
	OUT.c = IN.c;
	OUT.d = IN.d;
	float3 U, V;
	SurfAxes(IN.b.xyz, U, V);
	OUT.u = float4(U, IN.position.z);
	OUT.v = float4(V, IN.position.w);
	return OUT;
}

// The box round a patch (BloodSurface.h BuildBoxes: position xyz a corner along the patch's axes and normal, w its face:
// 0 -u, 1 +u, 2 -v, 3 +v, 4 -n, 5 +n). As CutBox: of each pair of faces the one past the eye is kept, so every pixel the
// box covers is shaded once, the eye in it or not.
SURFV SurfaceBoxVS(VSIN IN) {
	SURFV OUT;
	OUT.a = IN.a;
	OUT.b = IN.b;
	OUT.c = IN.c;
	OUT.d = IN.d;
	float3 U, V;
	SurfAxes(IN.b.xyz, U, V);
	OUT.u = float4(U, 0.0);
	OUT.v = float4(V, 0.0);
	float3 rel = IN.a.xyz + U * IN.position.x + V * IN.position.y + IN.b.xyz * IN.position.z;
	float face = floor(IN.position.w + 0.5);
	float axis = floor(face * 0.5);
	float side = face - axis * 2.0 > 0.5 ? 1.0 : -1.0;
	float3 out3 = axis < 0.5 ? U : (axis < 1.5 ? V : IN.b.xyz);
	OUT.position = side * dot(out3, rel) > 0.0 ? mul(float4(rel + TESR_BloodFlowShape.xyz, 1.0), TESR_ViewProjectionTransform) : float4(0.0, 0.0, -2.0, 1.0);
	return OUT;
}

float4 SurfRead(float2 texel) {
	return tex2Dlod(TESR_BloodSurface, float4((texel + 0.5) * TESR_BloodSurfaceSize.xy, 0.0, 0.0));
}

// The ground's fine unevenness at a point of the patch, which the depth buffer does not show (x: about -1 to 1: gentle dips
// and rises some 20 units across, bumps about 3 units across at half that height; nothing finer than two texels, which the
// grid would turn into dots), and how hard dry ground there holds a front back (y: 0-1). Fixed to the patch.
float2 SurfRelief(float2 p, float seed) {
	float2 wide = FlowNoise(float3(p * 0.05 + 3.1, seed * 61.0 + 1.9));
	float2 fine = FlowNoise(float3(p * 0.31, seed * 61.0));
	float hold = saturate(0.8 * (wide.y + fine.y) - 0.3);
	return float2(2.0 * (wide.x - 0.5) + (fine.x - 0.5), hold * hold);
}

// What the camera shows where the point P (eye-relative) would be: the pixel's centre, its surface point W (eye-relative)
// and a pixel's size there. False off the screen, behind the eye or on the sky.
bool SurfSee(float3 P, out float2 uv, out float3 W, out float pix) {
	float4 clip = mul(float4(P + TESR_BloodFlowShape.xyz, 1.0), TESR_ViewProjectionTransform);
	float2 s01 = clip.xy / max(clip.w, 1e-3) * float2(0.5, -0.5) + 0.5;
	uv = screenToRect(s01);
	uv = (floor(uv / TESR_ReciprocalResolution.xy) + 0.5) * TESR_ReciprocalResolution.xy;
	float depth = readDepthLod(uv);
	W = toWorld(uv) * depth;
	pix = depth * TESR_ReciprocalResolution.y * 2.0 / TESR_ProjectionTransform[1][1];
	return clip.w > nearZ && all(s01 > 0.002) && all(s01 < 0.998) && depth < farZ * 0.97;
}

// What a pixel is, from the material buffer: x 1 blood may lie on it (land, a rigid object, or no record: the game's own
// shaders), y 1 an actor (worn gear, skin) or cloth. Without the buffer: (1, 0).
float2 SurfClass(float2 uv) {
	float2 kind = float2(1.0, 0.0);
	[branch] if (TESR_MaterialBufferData.x > 0.5) {
		float4 material = tex2Dlod(TESR_MaterialBuffer, float4(uv, 0.0, 0.0));
		// (Resolved from the game's MSAA it holds averages, a the share of samples written: as WetWorld's GetSurfaceWetClass.)
#ifdef GBUFFER_V2
		// UNOFFICIAL 2026-11-14 GBufferV2 (core/CapturedNormals.h): r the shading model (+16 specular), written = r > 0.5 (a is a normal's y)
		float written = step(0.5, material.r);
		float k = floor(material.r + 0.01);
		k -= 16.0 * step(15.5, k);
		k = (k == 8.0 || k == 9.0) ? 2.0 : k;	// hair, eyes: an actor's, as worn gear
#else
		float share = clamp(material.a, 0.5, 1.0);
		float written = step(0.5, material.a) * step(0.5, material.r);
		float k = floor(material.r / share + 0.01);
#endif
		float ground = (k == 1.0 || k == 4.0) ? 1.0 : 0.0;
		float actor = (k == 2.0 || k == 3.0 || k == 6.0) ? 1.0 : 0.0;
		kind = written > 0.5 ? float2(ground, actor) : float2(1.0, 0.0);
	}
	float weapon = tex2Dlod(TESR_DepthBufferViewModel, float4(uv, 0.0, 0.0)).x;
	if ((TESR_DepthConstants.z == 0 && weapon < 0.9) || (TESR_DepthConstants.z > 0 && weapon > 0.01)) kind = float2(0.0, 1.0);
	return kind;
}

// The atlas keeps 11 bits of a number (fp16). A step changes a texel by far less than that (a source's edge, a slow creep,
// drying), and left to the card's own rounding those changes are lost or doubled step after step: a pool lost most of its
// blood in half a minute. So x is rounded here to a value fp16 holds exactly, up or down by chance (r: 0-1), which keeps the
// sums right on average.
float SurfKeep(float x, float r) {
	float ulp = exp2(floor(log2(max(x, 1e-6))) - 10.0);
	return floor(x / ulp + r) * ulp;
}

// One neighbour's part in this texel's step. flow: x what leaves this texel for it, y what comes from it, z that times
// how dry it was; known: the neighbours' heights that are known, weighted (x the sum, y the weights).
void SurfExchange(float4 me, float headMe, float pinMe, float2 origin, float2 t, float2 way, float w, float2 p, float dx,
	float2 fall, float kz, float seed, float rate, inout float3 flow, inout float2 known) {
	float2 tj = clamp(t + way, 0.0, TESR_BloodSurfaceSize.z - 1.0);
	float4 o = SurfRead(origin + tj);
	float2 pj = p + way * dx;
	float2 rj = SurfRelief(pj, seed);
	float headO = dot(pj, fall) + kz * (o.b + rj.x * TESR_BloodSurfaceSim.w + o.r);
	known += float2(o.b, 1.0) * step(0.4, o.a) * w;
	float dHead = headMe - headO;
	float outward = step(0.0, dHead);
	float4 up = lerp(o, me, outward);
	float4 down = lerp(me, o, outward);
	// (Dry ground holds the front back: the blood behind must stand this much higher.)
	float drive = max(abs(dHead) - step(down.r, SurfDryCell) * TESR_BloodSurfaceLook.y * lerp(pinMe, rj.y, outward), 0.0);
	float mobile = max(up.r - SurfRes, 0.0);
	float wet = 1.0 - up.g;
	// As a thin viscous film (thickness cubed); never more than this neighbour's share of what the texel holds; never past level.
	float F = min(min(rate * mobile * mobile * mobile * wet * wet * drive, mobile / 5.5), drive * 0.08 / kz) * w * step(0.4, down.a) * step(0.4, up.a);
	flow += float3(outward * F, (1.0 - outward) * F, (1.0 - outward) * F * o.g);
}

// The blood a splat or a body lays down at q (from its middle, along the patch), in thickness a second.
float SurfSource(float2 q, float3 n, float3 U, float3 V, float R, float seed, float pool, float age, float4 c, float4 d) {
	float rate = 0.0;
	[branch] if (pool > 0.5) {
		// A body: three spots under its middle, for BleedTime.
		[unroll] for (int k = 0; k < 3; k++) {
			float2 at = (float2(hash1(seed * 17.3 + k * 5.1), hash1(seed * 31.7 + k * 9.3)) * 2.0 - 1.0) * R * 0.3;
			float sigma = R * (0.1 + 0.07 * hash1(seed * 7.9 + k * 3.3));
			rate += exp(-dot(q - at, q - at) / (2.0 * sigma * sigma)) / (6.2832 * sigma * sigma * 3.0);
		}
		rate *= TESR_BloodSurfaceFlow.y * R * R / max(TESR_BloodSurfaceFlow.z, 0.1) * step(age, TESR_BloodSurfaceFlow.z);
	}
	else {
		// A splat: the game's own decal where its texture is known (its alpha), else a lumpy core with a few flecks round it.
		float cover = DecalCover(U * q.x + V * q.y, float4(n, R), c, d, 1.0);
		if (cover < -0.5) {
			float Rs = ShapeR(R);
			float r = length(q) / Rs;
			cover = saturate((SplatCoreRadius(atan2(q.y, q.x), seed, 1.0) * 1.3 - r) * 5.0)
				+ step(0.74, valueNoise(q * 0.45 + seed * 40.0)) * saturate(1.5 - r);
		}
		rate = TESR_BloodSurfaceFlow.x * saturate(cover * 2.0 - 0.4) * 2.0 * step(age, 0.5);
	}
	return rate;
}

float4 SurfaceSimPS(SURFP IN) : COLOR0 {
	float2 origin;
	float pool, reset, seen;
	SurfCode(IN.a.w, origin, pool, reset, seen);
	float N = TESR_BloodSurfaceSize.z;
	float2 t = floor(IN.vpos - origin + 0.25);		// this texel in its cell
	float halfSide = IN.b.w;
	float dx = 2.0 * halfSide / N;
	float2 p = ((t + 0.5) / N * 2.0 - 1.0) * halfSide;	// ... and on the patch, from its centre
	float3 n = IN.b.xyz;
	float3 U = IN.u.xyz, V = IN.v.xyz;
	float seed = ItemSeed(IN.c.z);
	float dt = TESR_BloodSurfaceSim.x;
	float4 me = SurfRead(origin + t) * (1.0 - reset);

	// Down the patch: a level patch has none of its own (only the heights count); on an upright one blood falls along it,
	// and what stands out of the wall counts for less.
	float2 fall = float2(U.z, V.z);
	float kz = max(n.z, 0.2);
	float2 relief = SurfRelief(p, seed);
	float headMe = dot(p, fall) + kz * (me.b + relief.x * TESR_BloodSurfaceSim.w + me.r);
	float rate = dt * TESR_BloodSurfaceSim.z * IN.c.y / (dx * dx);

	float3 flow = 0.0;
	float2 known = 0.0;
	SurfExchange(me, headMe, relief.y, origin, t, float2(1.0, 0.0), 1.0, p, dx, fall, kz, seed, rate, flow, known);
	SurfExchange(me, headMe, relief.y, origin, t, float2(-1.0, 0.0), 1.0, p, dx, fall, kz, seed, rate, flow, known);
	SurfExchange(me, headMe, relief.y, origin, t, float2(0.0, 1.0), 1.0, p, dx, fall, kz, seed, rate, flow, known);
	SurfExchange(me, headMe, relief.y, origin, t, float2(0.0, -1.0), 1.0, p, dx, fall, kz, seed, rate, flow, known);
	SurfExchange(me, headMe, relief.y, origin, t, float2(1.0, 1.0), 0.25, p, dx, fall, kz, seed, rate, flow, known);
	SurfExchange(me, headMe, relief.y, origin, t, float2(-1.0, 1.0), 0.25, p, dx, fall, kz, seed, rate, flow, known);
	SurfExchange(me, headMe, relief.y, origin, t, float2(1.0, -1.0), 0.25, p, dx, fall, kz, seed, rate, flow, known);
	SurfExchange(me, headMe, relief.y, origin, t, float2(-1.0, -1.0), 0.25, p, dx, fall, kz, seed, rate, flow, known);

	// The blood: what stays, what comes (with its own dryness), what the splat or body lays down (wet), what soaks away.
	float stays = max(me.r - flow.x, 0.0);
	float laid = SurfSource(p - float2(IN.u.w, IN.v.w), n, U, V, IN.c.w, seed, pool, IN.c.x, IN.c, IN.d) * dt * step(0.4, me.a);
	float h = stays + flow.y + laid;
	float dry = h > 1e-5 ? (stays * me.g + flow.z) / h : 0.0;
	h = h > SurfRes ? max(h - TESR_BloodSurfaceFlow.w * dt, SurfRes) : h;
	float dryTime = pool > 0.5 ? TESR_BloodFlowInfo.z : TESR_BloodFlowDry.w;
	dry = saturate(dry + dt / (dryTime * lerp(0.35, 1.6, saturate(h / TESR_BloodSurfaceLook.x))) * step(SurfDryCell, h));
	// (Blood that reaches the patch's edge is gone: it cannot be followed further.)
	if (any(t < 1.5) || any(t > N - 2.5)) h = 0.0;

	// The ground's height here.
	float z = me.b, k = me.a;
	[branch] if (TESR_BloodSurfaceSim.y > 0.5 && seen > 0.5) {
		float3 P0 = IN.a.xyz + U * p.x + V * p.y;
		float z0 = k > 0.4 ? z : (known.y > 0.0 ? known.x / known.y : 0.0);
		float zc = z0;
		float2 uv;
		float3 W;
		float pix;
		bool on = SurfSee(P0 + n * zc, uv, W, pix);
		// (What the camera shows where this ground should be: how far it stands out of the patch, and whether it is an actor.)
		float first = dot(W - IN.a.xyz, n) - z0;
		float firstActor = SurfClass(uv).y;
		zc = clamp(first + z0, -halfSide, halfSide);
		[unroll] for (int i = 0; i < 2; i++) {
			on = SurfSee(P0 + n * zc, uv, W, pix) && on;
			zc = clamp(dot(W - IN.a.xyz, n), -halfSide, halfSide);
		}
		on = SurfSee(P0 + n * zc, uv, W, pix) && on;
		float3 off = W - P0;
		float zHit = dot(off, n);
		float miss = length(off - n * zHit);
		// (The point found is the middle of a pixel, a little to the side of this texel: its height is carried over along the
		// surface's own slope there. Without that the pixel grid shows in the heights as a pattern of dots.)
		float3 Ns = normalize(GetWorldNormalLod(uv));
		float facing = dot(n, Ns);
		if (facing > 0.35) zHit = dot(off, Ns) / facing;
		float2 kind = SurfClass(uv);
		// (A face that stands on the patch, a crate's side, is no ground: its texels would hold whatever height the walk ended at.)
		bool hit = on && miss < max(0.7 * dx, 2.0 * pix) && abs(zHit) < 0.75 * halfSide && facing > 0.2;
		if (hit && kind.x > 0.5) {
			// (Averaged over its first 8 reads, then followed slowly: a jittered depth buffer moves it a little every frame.)
			float reads = k >= 1.0 ? k : 0.0;
			z = lerp(z, zHit, 1.0 / (reads + 1.0));
			k = min(reads + 1.0, 8.0);
		}
		// Something tall and solid stands here, or hides this ground (a wall, a crate; never an actor: it will move on).
		else if (on && !hit && kind.y < 0.5 && firstActor < 0.5 && k < 0.9 && first > SurfTall) k = 0.25;
	}
	// Not read yet: as the texels round it (their mean, again every step: it settles to a smooth sheet between them).
	if (k < 0.1 || (k > 0.4 && k < 0.9)) {
		if (known.y > 0.0) { z = known.x / known.y; k = 0.5; }
	}
	float chance = hash2(t + frac(IN.c.x * 7.31) * 613.0);
	return float4(SurfKeep(h, chance), SurfKeep(dry, frac(chance * 57.0 + 0.37)), z, k);
}

float4 SurfacePS(SURFP IN) : COLOR0 {
	float2 uv = (IN.vpos + 0.5) * TESR_ReciprocalResolution.xy;
	float depth = readDepth(uv);
	if (depth >= farZ * 0.979) discard;
	float3 rel = toWorld(uv) * depth;
	float3 pp = rel - IN.a.xyz;
	float3 n = IN.b.xyz;
	float3 U = IN.u.xyz, V = IN.v.xyz;
	float halfSide = IN.b.w;
	float2 p = float2(dot(pp, U), dot(pp, V));
	if (any(abs(p) > halfSide)) discard;
	float2 origin;
	float pool, reset, seen;
	SurfCode(IN.a.w, origin, pool, reset, seen);
	float N = TESR_BloodSurfaceSize.z;
	float dx = 2.0 * halfSide / N;
	float2 tc = clamp((p / halfSide * 0.5 + 0.5) * N, 2.0, N - 2.0);
	float2 at = (origin + tc) * TESR_BloodSurfaceSize.xy;
	float debug = TESR_BloodSurfaceLook.z;
	// (Four reads between the texels round the point cover the nine it can take blood from: nothing there, nothing to draw.)
	float2 e = TESR_BloodSurfaceSize.xy * 0.5;
	float near4 = tex2Dlod(TESR_BloodSurface, float4(at + float2(-e.x, -e.y), 0.0, 0.0)).r + tex2Dlod(TESR_BloodSurface, float4(at + float2(e.x, -e.y), 0.0, 0.0)).r
		+ tex2Dlod(TESR_BloodSurface, float4(at + float2(-e.x, e.y), 0.0, 0.0)).r + tex2Dlod(TESR_BloodSurface, float4(at + float2(e.x, e.y), 0.0, 0.0)).r;
	if (near4 < 1e-4 && debug < 0.5) discard;

	// The blood here: the nine texels round the point, weighted as a smooth bump over them (a quadratic spline: the outline
	// comes out rounded, not as texel steps), but only the texels this pixel's surface point lies on: each texel has its
	// own height, and a texel counts if the point is within what ground can rise from there to here, plus a pixel's size
	// (looser where the height is only a guess). So the blood on the ground beside a crate or a kerb does not spread onto
	// its top or side, something standing in or passing through the patch matches no texel and is not painted, and the
	// ground right up to it keeps its blood.
	float2 mid = floor(tc);
	float2 f = tc - mid - 0.5;
	float3 wx = float3(0.5 * (0.5 - f.x) * (0.5 - f.x), 0.75 - f.x * f.x, 0.5 * (0.5 + f.x) * (0.5 + f.x));
	float3 wy = float3(0.5 * (0.5 - f.y) * (0.5 - f.y), 0.75 - f.y * f.y, 0.5 * (0.5 + f.y) * (0.5 + f.y));
	float3 dwx = float3(f.x - 0.5, -2.0 * f.x, f.x + 0.5), dwy = float3(f.y - 0.5, -2.0 * f.y, f.y + 0.5);
	float zp = dot(pp, n);
	float aa = max(depth * TESR_ReciprocalResolution.y * 2.0 / TESR_ProjectionTransform[1][1], 0.05);
	float4 s = 0.0;			// r thickness, g dryness x thickness, b weights that count, a the least known of them
	float4 slopes = 0.0;	// xy the blood's, zw the ground's, per texel
	s.a = 8.0;
	float4 centre = 0.0;
	[unroll] for (int j = 0; j < 3; j++) {
		[unroll] for (int i = 0; i < 3; i++) {
			float4 t = SurfRead(origin + mid + float2(i - 1.0, j - 1.0));
			if (i == 1 && j == 1) centre = t;
			float reach = length(float2(i - 1.0, j - 1.0) - f) * dx;
			float m = (t.a > 0.4 && abs(zp - t.b) < 0.8 + 2.0 * aa + 3.0 * reach + (t.a < 0.99 ? 2.5 : 0.0)) ? 1.0 : 0.0;
			float w = wx[i] * wy[j] * m;
			s.rgb += float3(t.r, t.g * t.r, 1.0) * w;
			s.a = m > 0.5 ? min(s.a, t.a) : s.a;
			slopes += float4(t.r, t.r, t.b - zp, t.b - zp) * float4(dwx[i] * wy[j], wx[i] * dwy[j], dwx[i] * wy[j], wx[i] * dwy[j]) * m;
		}
	}
	float onIt = saturate(s.b * 2.0);
	s.r /= max(s.b, 0.05);
	s.g /= max(s.r * max(s.b, 0.05), 1e-5);
	float2 gh = slopes.xy / (max(s.b, 0.05) * dx);
	float2 gz = slopes.zw / (max(s.b, 0.05) * dx);
	float cover = smoothstep(SurfDryCell, 0.014, s.r);
	if (cover * IN.c.y < 0.004 && debug < 0.5) discard;
	// Only ground or a rigid object, and only what faces the way the patch does (not the side of something standing on it).
	float2 kind = SurfClass(uv);
	float3 Ns = normalize(GetWorldNormalLod(uv));
	float keep = onIt * kind.x * saturate((dot(Ns, n) - 0.2) / 0.15);

	[branch] if (debug > 0.5) {
		// Debug view 1: blood red (brighter = thicker) on what the patch knows of the ground: green read, yellow guessed, blue
		// something tall, grey nothing; magenta: blood there, but this pixel is not its surface (something in the way).
		// 2: the height it holds, as bands every 2 units.
		float3 g = centre.a > 0.99 ? float3(0.0, 0.5, 0.0) : centre.a > 0.4 ? float3(0.6, 0.6, 0.0) : centre.a > 0.2 ? float3(0.0, 0.2, 0.9) : float3(0.3, 0.3, 0.3);
		if (debug > 1.5) g = frac(centre.b * 0.5) < 0.5 ? g : g * 0.5;
		float3 colour = lerp(g, float3(0.4 + 0.6 * saturate(s.r / TESR_BloodSurfaceLook.x), 0.0, 0.0), cover);
		if (keep < 0.5) colour = cover > 0.5 ? float3(1.0, 0.0, 1.0) : colour * 0.3;
		if (onIt < 0.5 && cover < 0.5) discard;
		return float4(colour, 0.8);
	}
	// (It thins out toward the patch's edge: blood that runs that far cannot be followed further.)
	float amount = cover * keep * IN.c.y * saturate((halfSide - max(abs(p.x), abs(p.y))) / (0.1 * halfSide));
	if (amount < 0.004) discard;

	float4 scene = tex2Dlod(TESR_SourceBuffer, float4(uv, 0.0, 0.0));
	float3 sceneLin = linearize(max(scene.rgb, 0.0));
	// The light that is already on this pixel: the picture over the surface's own colour (its record; else a typical grey).
	float3 surface = AssumedAlbedo;
	float own = 1.0;	// the blood's own colour, x (SurfTypicalAlbedo, above)
	[branch] if (TESR_AlbedoBufferData.x > 0.5 && TESR_BloodSurfaceLook.w > 0.5) {
		float4 record = tex2Dlod(TESR_AlbedoBuffer, float4(uv, 0.0, 0.0));
		if (record.a > 0.5) {
			surface = clamp(record.rgb / clamp(record.a, 0.5, 1.0), 0.02, 1.0);
			own = SurfTypicalAlbedo / AssumedAlbedo;
		}
	}
	float3 light = min(sceneLin / surface, 8.0 / own);
	float lightLevel = min(luma(light) * own, 2.5);	// (on the scale the highlights were set against)

	float thick = saturate(s.r / TESR_BloodSurfaceLook.x);
	float dryTime = pool > 0.5 ? TESR_BloodFlowInfo.z : TESR_BloodFlowDry.w;
	float dry = saturate(s.g + IN.c.x / (dryTime * lerp(0.35, 1.6, thick)));
	// The blood's own colour (as BloodPS: the settings', or the game's decal colour with MatchGameColor).
	float3 freshColour = TESR_BloodFlowFresh.rgb;
	float3 dryColour = TESR_BloodFlowDry.rgb;
	float3 hue = float3(1.0, 0.0, 0.0);
	[branch] if (TESR_BloodFlowMatch.w > 0.5 && IN.d.w > 0.5) {
		float tw = floor(IN.d.w + 0.5);
		float r = floor(tw / 65536.0);
		r += (tw - r * 65536.0 >= 65536.0) ? 1.0 : ((tw - r * 65536.0 < 0.0) ? -1.0 : 0.0);
		float gb = tw - r * 65536.0;
		float g = floor(gb / 256.0);
		g += (gb - g * 256.0 >= 256.0) ? 1.0 : ((gb - g * 256.0 < 0.0) ? -1.0 : 0.0);
		freshColour = linearize(saturate(float3(r, g, gb - g * 256.0) / 255.0 * TESR_BloodFlowMatch.x)) * TESR_BloodFlowMatch.y;
		freshColour *= max(1.0, TESR_BloodFlowLook.w / max(max(freshColour.r, max(freshColour.g, freshColour.b)), 1e-4));
		dryColour = freshColour * float3(0.72, 0.8, 0.8) + freshColour.r * float3(0.0, 0.1, 0.05);
		hue = freshColour / max(max(freshColour.r, max(freshColour.g, freshColour.b)), 1e-4);
	}
	float3 albedo = lerp(freshColour, dryColour, dry);
	albedo *= lerp(1.0, 0.8 + 0.4 * valueNoise(p * 0.35 + ItemSeed(IN.c.z) * 31.0), dry);
	// A film of blood over the surface: the surface shows through it, less the thicker it is and least in the colours blood
	// soaks up; the rest is the same light scattered back by the blood itself (as BloodPS's sub-surface look).
	// (BloodPS's pools start at 0.35 of its thickness scale: the thinnest whole film here, the stain blood leaves behind, likewise
	// starts at 0.3, or it reads as watercolour.)
	float body = lerp(0.3, 1.0, thick);
	float3 through = exp(-(1.2 + log(max(max(albedo.r, max(albedo.g, albedo.b)), 0.001) / max(albedo, 0.001))) * (0.25 + 2.0 * body));
	float rim = saturate(length(gh) * 6.0);		// its edge, where it thins out
	float3 blood = (sceneLin * through + light * albedo * own * lerp(1.0, 0.6, saturate(body * 1.3 - 0.3)) * (1.0 - through)) * (1.0 - 0.35 * dry * rim);

	// Wet blood shines: the sun where this pixel is not in its shadow, the lamps, a little of the sky or the room at low
	// angles; all of it fading as the blood dries. Thick blood lies level over the bumps under it, and bulges at its edge.
	float gloss = TESR_BloodFlowFresh.w * (1.0 - dry) * lerp(0.5, 1.0, thick);
	[branch] if (gloss > 0.001) {
		float3 rd = normalize(rel);
		float3 Nb = normalize(lerp(Ns, n, saturate(thick * 1.5)) - (U * gh.x + V * gh.y) * 3.0);
		float rough = lerp(0.12, 0.45, dry);
		float a2 = rough * rough * rough * rough;
		float3 glints = 0.0;
		if (TESR_BloodFlowInfo.x > 0.5) {
			float sunAmbient = max(luma(linearize(TESR_SunAmbient.rgb)), 0.001);
			float4 shadows = tex2Dlod(TESR_PointShadowBuffer, float4(uv, 0.0, 0.0));
			float lit = saturate(pow(abs((shadows.r + shadows.g) / sunAmbient), 5.0));
			glints += Glint(Nb, -rd, normalize(TESR_SunDirection.xyz), a2) * lit * linearize(saturate(TESR_SunColor.rgb));
		}
		float3 P = TESR_CameraPosition.xyz + rel;
		[loop] for (int i = 0; i < 12; i++) {
			glints += LampGlint(Nb, -rd, P, TESR_ShadowLightPosition[i], TESR_LightColor[i].rgb, a2) * saturate(TESR_LightColor[i].rgb);
			glints += LampGlint(Nb, -rd, P, TESR_LightPosition[i], TESR_LightColor[i + 12].rgb, a2) * saturate(TESR_LightColor[i + 12].rgb);
		}
		float fresnel = 0.02 + 0.98 * pow(1.0 - saturate(dot(Nb, -rd)), 5.0);
		float3 surroundings = TESR_BloodFlowInfo.x > 0.5 ? linearize(saturate(TESR_HorizonColor.rgb)) * min(lightLevel, 1.0) : light * own * 0.1;
		blood += (min(glints, 1.0) * 0.4 + min(fresnel, TESR_BloodFlowLook.y) * surroundings * lerp(1.0, hue, TESR_BloodFlowLook.z) * (1.0 - rough) * 0.5 * TESR_BloodFlowLook.x) * gloss;
	}
	// (As BloodPS: this runs on the picture before the tonemap; never brighter than the brighter of 1 and the scene here.)
	float ceiling = max(max(sceneLin.r, max(sceneLin.g, sceneLin.b)), 1.0);
	blood *= ceiling / max(max(blood.r, max(blood.g, blood.b)), ceiling);
	return float4(delinearize(max(blood, 0.0)), amount);
}

// A step of the simulation for the patches' cells (one quad each), from one copy of the atlas into the other.
technique BloodSurfaceSim
{
	pass
	{
		VertexShader = compile vs_3_0 SurfaceSimVS();
		PixelShader = compile ps_3_0 SurfaceSimPS();
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

// The patches' blood, blended into the image.
technique BloodSurface
{
	pass
	{
		VertexShader = compile vs_3_0 SurfaceBoxVS();
		PixelShader = compile ps_3_0 SurfacePS();
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

#endif
