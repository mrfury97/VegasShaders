// UNOFFICIAL 2026-10-16 cloth wind ([Shaders.Cloth.Main], core/ClothWind.h): capes, dusters, flags, banners, curtains move in the
// wind by maths in the vertex shader alone -- no physics, no CPU work per vertex. Which objects: textures whose path holds one of
// the Keywords (per draw, by NVR). Vertex shaders only (ObjectTemplate, Shadows\ShadowMap.vso).
//
// Per draw NVR sets (vertex shader constants; strength 0 = this draw is not cloth and nothing below runs):
//   c248 ClothA: x strength 0-1, y mask (0 vertex colour RED: 1 = loose, 0 = pinned; 1 = whole mesh; 2 = by height: pinned at the
//                top, loosest at the bottom -- for meshes without an authored red mask; 3 = the same by the vertex's BIND-POSE
//                height (the mesh's own position before skinning: worn clothing; independent of the pose, the actor's place and
//                the space the bones put it in); the fraction above 3 is the height (/1000) below which legs and boots stay still), z ripple 0-1, w flutter 0-1
//   c249 ClothB: xyz wind direction in the vertex shader's space (unit), w cycles per unit along the wind (1 / wavelength)
//   c250 ClothC: x main wave phase, y gust phase, z/w flutter phases -- in cycles, wrapped on the CPU (double precision), so the
//                motion never jitters after long play
//   c251 ClothD: x intensity (units), y ripple cycles per unit, z top height (mask 2), w 1 / (top - bottom) (mask 2)
//   c252 ClothE: (2026-10-19) worn clothing follows its wearer: xyz the offset (units: trailing behind the movement + the stride
//                swish) -- 0 for everything but worn clothing
//   c253 ClothF: the lag behind a turn as a small turn of the garment's bind-pose position around its upright axis, carried into
//                the vertex shader's space by the wearer's heading: turn = bind.x * xy + bind.y * zw (x/y only)
//   c226-c237 ClothLimb (2026-10-19, mask 4 = worn clothing by its own skeleton): the garment's hip, knee, ankle, toe tip (left,
//                right) and elbow, wrist (left, right) where its bones sat when it was made (the mesh's own space, as IN.position),
//                w the limb's radius there;  c238 ClothLimbParams: x hip height, y 1 / (hip - knee height), z gap off the limb where
//                cloth starts to move, w 1 / fade;  c239 ClothLimbGround: x the height below which nothing moves, y 1 / its fade,
//                zw (mask 5) the wearer's upright axis in the bind pose (between the hip joints)
//   mask 5 (2026-10-19 round 5, worn clothing): the per-vertex weight NVR worked out from the garment's own mesh (core/ClothWeights.h:
//                trouser legs and boots 0, the coat pinned at the hips and loosest at the hem, its shell and lining averaged together),
//                read from vertex stream 1 (TEXCOORD7, bound only for those draws: core/ClothWind.h DrawPartition); the billow part of
//                the wind pushes AWAY FROM THE WEARER'S UPRIGHT AXIS instead of along the vertex normal -- a lining's normal points
//                inward, so the normal billow pushed shell and lining apart (up to ~17 units in a gale). The caller passes that
//                direction already carried into the skinned space (ClothOutwardBind, through the bones).
//                (2026-10-19 round 6) That billow is ONE-SIDED: outward as before, inward softly limited to ClothBillowIn units --
//                it swung both ways by up to (ripple + flutter) x intensity, and its inward half pushed the coat into the legs (60 units
//                at 30 units of wind with ripple and flutter 1). The weight already scales with the coat's clearance from the body.
//                (2026-10-19 round 7) The same soft limit on the inward part of the WHOLE displacement (push + billow + the wearer's
//                motion): the push along the wind and the motion trail are two-way too, and on the windward side they still pressed
//                the coat 4-8 units into the legs at 30 units of wind. Sideways (around the wearer) and outward stay as they were.
//                (2026-10-19 round 8) Then a limit per LEG (thigh / calf axis, ClothLimb, the garment's measured radius): 'towards the
//                wearer's axis' alone let a panel in front of a knee but off the centre line slide sideways onto that knee (offline: a
//                BoS scribe robe panel 1.1 units into its leg core). It lets the cloth go towards a leg only as far as it still ends
//                ClothLegMargin off the leg's surface (ClothLegLimit). The legs are worked out in the bind pose and carried into the
//                skinned space by the same bones as the billow's direction (the caller passes them).
//   (2026-10-20 round 9) the walking simulation (core/ClothWalk.h, core/ClothWind.h WalkOf / WalkConstants), when c219.x is 1: the
//                garment's own skin weights glue a coat's skirt to the leg bones (it moved like trousers), so the caller lets each coat
//                vertex HANG FROM THE HIPS -- its skinned place blended towards the pelvis bone's alone (c200-c202) by weight x hang
//                (c219.y) -- adds the hem ring's displacement at the vertex's angle (c203-c218, between the two nearest of 16 points;
//                stream 1's second float is the vertex's place on the ring) under the weight like the wearer's motion, works round 8's
//                leg limits out against the CURRENT legs (c220-c225, xyz in the shader's space, w the garment's measured radius), and
//                then keeps the vertex out of those legs (ClothLegPush): pushed OUTWARD (away from the wearer's axis, the same way for a
//                shell and its lining) across the leg, smoothly (nothing well outside, never more for a point further out: layers keep
//                their order), on a leg's inner half only once inside it. Offline: bench\deep-roadmap\evidence\cloth3\cloth_walk_check.py.
//                (2026-10-20 round 9b) The ring moves each vertex by its PENDULUM share (stream 1's third float: 0 at the hips rising
//                evenly to 1 at the hem), added after the wind -- by the cloth weight (full from the knee down) the lower coat moved as
//                one stiff block and folded at the knee; and where the outward direction runs along a leg (a calf kicked up level
//                behind at a run) the push goes straight off the leg instead (radially: over the calf).
//   mask 6 (2026-10-20, still cloth: tents, flags, banners; user: 'the entire tent mesh wobbles around'): the per-vertex weight NVR
//                worked out from the still mesh's own shape (core/ClothStatic.h: poles, ropes and solid parts 0, the cloth held where it
//                touches them, the ground and its top line, loosest at its free edges), read from vertex stream 1 like mask 5 (TEXCOORD7:
//                weight, the billow's side); the billow goes along the normal x that side (+1 / -1), so the two layers of two-sided cloth
//                (whose normals face apart) billow the same way. The callers pass the side as out5.x. Mask 2 (by height) stays for
//                StillWeights off: it moved the bottom most -- a tent's poles, its ropes and the canvas on the ground.
// The same function runs in every pass of the object (the extra lamp passes test depth EQUAL, so they must land on the same spot)
// and in the shadow maps.
//
// Wave shape: Crytek's smooth triangle wave (Sousa, "Vegetation Procedural Animation and Shading in Crysis", GPU Gems 3 ch. 16,
// 2007) -- cheaper than sin() and smooth. Normals: rebuilt from two moved neighbours (finite differences along the tangent and
// the binormal), so lighting and shadows follow the folds.
// 2026-10-20: Includes/PlantWind.hlsl reuses ClothA-F (the same registers) only where this file is included (it is not in the
// SpeedTree leaf shaders: with PLANT_WIND_ON and CLOTH_WIND_ON both on they failed to compile and the leaves flew about).
#define CLOTH_WIND_DECLARED
float4 ClothA : register(c248);
float4 ClothB : register(c249);
float4 ClothC : register(c250);
float4 ClothD : register(c251);
float4 ClothE : register(c252);
float4 ClothF : register(c253);
float4 ClothLimb[12] : register(c226);
float4 ClothLimbParams : register(c238);
float4 ClothLimbGround : register(c239);
float4 ClothPelvis[3] : register(c200);	// round 9: the pelvis bone, bind pose -> the shader's space (rows)
float4 ClothRing[16] : register(c203);		// round 9: the hem ring's displacements (the shader's space)
float4 ClothWalk : register(c219);			// round 9: x 1 = the walking simulation is on for this draw, y hang (0..1)
float4 ClothLegNow[6] : register(c220);		// round 9: the current legs: L hip, knee, ankle, R hip, knee, ankle (xyz, radius)
static float3 ClothWalkMove = 0.0f;			// round 9: added to the wearer's motion (round 9b: unused -- the ring goes by the pendulum share, see the callers)

float ClothTri(float c) {	// 0..1, period 1, smooth at the turns
    const float t = abs(frac(c + 0.5f) * 2.0f - 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Mask 5's billow, and (round 7) its whole displacement: the most it pulls cloth inward (units, before the weight). With the weight's
// clearance factor (core/ClothWeights.h) inward stays below the straight-line room to the trousers (checked offline:
// bench\deep-roadmap\evidence\cloth3-LOG.md C3-12, 2 was 0.01 units too much; C3-13 for the whole displacement).
static const float ClothBillowIn = 1.5f;

// Mask 5 (worn clothing by its own mesh) alone; mask 6 (still cloth) shares only its stream weight (ClothMask).
bool ClothIsMask5() { return ClothA.x > 0.0f && ClothA.y > 4.5f && ClothA.y < 5.5f; }
// The billow's direction: mask 5 away from the wearer's axis (out5), mask 6 along the normal x its side (out5.x), else the normal.
float3 ClothBillowDir(float3 n, float3 out5) {
    return ClothA.y > 5.5f ? n * (out5.x < 0.0f ? -1.0f : 1.0f) : (ClothIsMask5() ? out5 : n);
}

// The displacement at point p (the shader's space) with surface normal n; n = 0 gives the push along the wind only.
float3 ClothDisplacement(float3 p, float3 n) {
    const float along = dot(p, ClothB.xyz) * ClothB.w;
    const float wave = ClothTri(along - ClothC.x) * 2.0f - 1.0f;                                      // billow, travels with the wind
    const float gust = 0.7f + 0.3f * ClothTri(ClothC.y + dot(p, float3(0.0021f, 0.0027f, 0.0013f)));   // slow, varies over the cloth
    const float r = ClothD.y;
    const float ripple = 0.6f * (ClothTri(dot(p, float3(0.71f, 0.53f, 0.37f)) * r + ClothC.z) * 2.0f - 1.0f)
                       + 0.4f * (ClothTri(dot(p, float3(-0.43f, 0.81f, 0.59f)) * r * 1.7f - ClothC.w) * 2.0f - 1.0f);
    float billow = ClothA.z * wave + ClothA.w * ripple;
    // Mask 5 (2026-10-19 round 6): one-sided. In units b = intensity x gust x billow: outward unchanged, inward b / sqrt(1 + (b / In)^2)
    // (slope 1 at rest, so small motion looks as before; never deeper than ClothBillowIn). Every other mask keeps its billow exactly.
    const float in5 = min(billow, 0.0f);
    const float k5 = ClothD.x * gust * (1.0f / ClothBillowIn);
    [flatten] if (ClothIsMask5()) billow += in5 * (rsqrt(1.0f + in5 * in5 * k5 * k5) - 1.0f);
    return ClothD.x * gust * (ClothB.xyz * (0.6f + 0.4f * wave) + n * billow);
}

// The wearer's movement (worn clothing; core/ClothWind.h MotionOf): the same offset everywhere, plus a small turn of the garment
// around its own upright axis (from the bind-pose position, so it does not depend on any camera). Under the same mask as the wind.
float3 ClothMotion(float3 bindP) {
    return ClothE.xyz + float3(bindP.x * ClothF.xy + bindP.y * ClothF.zw, 0.0f) + ClothWalkMove;
}

// Round 9: the pelvis bone's transform of a bind-pose point / vector, and the hem ring at a place u (point i at u = i, 0..16).
float3 ClothPelvisPoint(float3 bindP) {
    const float4 q = float4(bindP, 1.0f);
    return float3(dot(ClothPelvis[0], q), dot(ClothPelvis[1], q), dot(ClothPelvis[2], q));
}
float3 ClothPelvisVector(float3 v) {
    return float3(dot(ClothPelvis[0].xyz, v), dot(ClothPelvis[1].xyz, v), dot(ClothPelvis[2].xyz, v));
}
float3 ClothRingAt(float u) {
    const float i0 = floor(u);
    const float i1 = i0 >= 15.0f ? 0.0f : i0 + 1.0f;
    return lerp(ClothRing[(int)i0].xyz, ClothRing[(int)i1].xyz, u - i0);
}
static const float ClothPushMargin = 1.0f;	// units the coat is kept off the measured leg surface
static const float ClothPushBand = 2.0f;	// on a leg's inner half: fades in over this depth
static const float ClothPushSoft = 2.0f;	// units: how softly the push starts (2026-10-20 round 9c: 1 -> 2, fewer one-row folds where a leg presses)
// One leg segment (a, b: xyz, radius): the push across the leg in the outward direction o (cloth_walk_check.py new_coat).
float3 ClothLegPushSegment(float3 p, float3 o, float4 a, float4 b) {
    const float3 ab = b.xyz - a.xyz;
    const float L = length(ab);
    const float3 e = ab / max(L, 1e-4f);
    const float3 rel = p - a.xyz;
    const float along = dot(rel, e);
    const float R = lerp(a.w, b.w, saturate(along / max(L, 1e-4f))) + ClothPushMargin;
    const float3 pp = rel - along * e;
    const float3 op = o - dot(o, e) * e;
    const float ol = length(op);
    const float3 oh = op / max(ol, 1e-4f);
    const float B = dot(pp, oh);
    const float disc = B * B - (dot(pp, pp) - R * R);
    const float sexit = disc >= 0.0f ? sqrt(max(disc, 0.0f)) - B : -1000.0f;
    const float depth = R - length(pp);
    const float gin = depth > 0.0f ? smoothstep(0.0f, 1.0f, depth * (1.0f / ClothPushBand)) : 0.0f;
    const float g = gin + (1.0f - gin) * smoothstep(0.0f, 1.0f, (B + 1.0f) * 0.5f);
    const float ends = saturate(1.0f - max(-along, along - L) / R);
    const float ko = smoothstep(0.0f, 1.0f, (ol - 0.1f) * (1.0f / 0.3f));
    const float lp = length(pp);
    // round 9b: where o runs along the leg, straight off it (radially from its axis), the same softplus of depth
    const float mr = ClothPushSoft * log(1.0f + exp(min((R - lp) * (1.0f / ClothPushSoft), 40.0f))) * ends * (1.0f - ko);
    return oh * (ClothPushSoft * log(1.0f + exp(min(sexit * (1.0f / ClothPushSoft), 40.0f))) * g * ends * ko) + pp / max(lp, 1e-4f) * mr;
}
// Out of the current legs (the largest of the four pushes: each thigh and calf).
float3 ClothLegPush(float3 p, float3 o) {
    float3 best = ClothLegPushSegment(p, o, ClothLegNow[0], ClothLegNow[1]);
    float3 q = ClothLegPushSegment(p, o, ClothLegNow[1], ClothLegNow[2]);
    best = dot(q, q) > dot(best, best) ? q : best;
    q = ClothLegPushSegment(p, o, ClothLegNow[3], ClothLegNow[4]);
    best = dot(q, q) > dot(best, best) ? q : best;
    q = ClothLegPushSegment(p, o, ClothLegNow[4], ClothLegNow[5]);
    return dot(q, q) > dot(best, best) ? q : best;
}

// Mask 5 (2026-10-19 round 7): the unit direction away from the wearer's upright axis for ClothInward5, 0 for every other mask (a select:
// there ClothInward5 gives d back as it was).
float3 ClothInDir(float3 out5) {
    return ClothIsMask5() ? out5 * rsqrt(max(dot(out5, out5), 1e-8f)) : 0.0f;
}

// The whole displacement d (wind + the wearer's motion, units, before the weight) with its part towards the wearer's upright axis (along
// -in5) softly limited as the billow's: s / sqrt(1 + (s / ClothBillowIn)^2), slope 1 at rest, never deeper than ClothBillowIn.
float3 ClothInward5(float3 d, float3 in5) {
    const float s = min(dot(d, in5), 0.0f);
    return d + in5 * (s * (rsqrt(1.0f + s * s * (1.0f / (ClothBillowIn * ClothBillowIn))) - 1.0f));
}

// Mask 5 (2026-10-19 round 8): ONE LIMIT PER LEG. Per leg, in the bind pose (ClothLimb: hip, knee, ankle; w the garment's MEASURED
// radius): the vector from the leg's axis to the vertex and the radius there; carried into the skinned space by the vertex's bones (as
// out5), so its length is the distance from the axis in that space. The limit lets d's part towards that axis (along -n) go as far as
// the vertex still ENDS ClothLegMargin off the measured surface, given how far it moves around the leg (T: neither towards it nor along
// it -- a flap swinging past a leg is not one swinging into it): x <= r - sqrt((R + margin)^2 - T^2), never less than ClothBillowIn;
// soft knee s / (1 + (s / allowance)^4)^(1/4) (keeps the motion below the allowance as it was). Left, right, twice (a correction for
// one leg can only push towards the other between the legs). Offline: bench\deep-roadmap\evidence\cloth3-LOG.md C3-14.
static const float ClothLegTau = 2.0f;		// units: the thigh and calf blend near the knee (a smooth minimum of their distances)
static const float ClothLegMargin = 1.0f;	// units the cloth stays off the measured leg surface

// One leg segment (a-b: hip-knee or knee-ankle) seen from p: xyz from the nearest point of its axis to p, w the distance off its surface.
float4 ClothLegSegment(float3 p, float4 a, float4 b) {
    const float3 ab = b.xyz - a.xyz;
    const float t = saturate(dot(p - a.xyz, ab) / max(dot(ab, ab), 1e-4f));
    const float3 v = p - (a.xyz + ab * t);
    return float4(v, length(v) - lerp(a.w, b.w, t));
}

// One leg in the bind pose: xyz = the unit direction away from its axis (thigh and calf blended by 2^(-extra distance / ClothLegTau))
// times the (blended) distance r from the axis; w = the radius there / r (so the skinned radius is |xyz| x w).
float4 ClothLegBind(float3 bindP, float4 hip, float4 knee, float4 ankle) {
    const float4 t = ClothLegSegment(bindP, hip, knee);
    const float4 c = ClothLegSegment(bindP, knee, ankle);
    const float2 r = float2(length(t.xyz), length(c.xyz));
    const float2 e = exp2((min(t.w, c.w) - float2(t.w, c.w)) * (1.0f / ClothLegTau));
    const float3 g = e.x * t.xyz / max(r.x, 1e-4f) + e.y * c.xyz / max(r.y, 1e-4f);
    const float rb = dot(e, r) / (e.x + e.y);
    const float sb = dot(e, float2(t.w, c.w)) / (e.x + e.y);
    return float4(g * (rsqrt(max(dot(g, g), 1e-8f)) * rb), (rb - sb) / max(rb, 1e-3f));
}

// The limit for one leg (leg = ClothLegBind carried by the bones, w as it was; up = the bind pose's up carried alike: the leg's direction;
// mask = how much of d the vertex moves).
float3 ClothLegLimit(float3 d, float4 leg, float3 up, float mask) {
    const float r = length(leg.xyz);
    const float3 n = leg.xyz / max(r, 1e-4f);
    const float3 a = up - dot(up, n) * n;
    const float dn = dot(d, n);
    const float da = dot(d, a) * rsqrt(max(dot(a, a), 1e-8f));
    const float T2 = max(dot(d, d) - dn * dn - da * da, 0.0f) * (mask * mask);
    const float R = r * leg.w + ClothLegMargin;
    const float allow = max(ClothBillowIn, (r - sqrt(max(R * R - T2, 0.0f))) / max(mask, 1e-3f));
    const float s = min(dn, 0.0f);
    const float q = s / allow;
    return d + n * (s * rsqrt(sqrt(1.0f + q * q * q * q)) - s);
}

// Mask 5: the wearer's axis first (round 7), then each leg (round 8). Every other mask: d exactly -- the callers fill the legs only
// for mask 5 (w = radius / distance > 0), so the leg part is not run (and the unskinned shaders, passing literal zeros, do not carry it).
float3 ClothLimit5(float3 d, float3 in5, float4 legL, float4 legR, float3 up, float mask) {
    d = ClothInward5(d, in5);
    [branch] if (legL.w + legR.w > 0.0f) {
        [unroll] for (int i = 0; i < 2; i++) {
            d = ClothLegLimit(d, legL, up, mask);
            d = ClothLegLimit(d, legR, up, mask);
        }
    }
    return d;
}

// Mask 4: distance from bindP to the limb segment a-b, minus the limb's radius there.
float ClothLimbDistance(float3 p, float4 a, float4 b) {
    const float3 ab = b.xyz - a.xyz;
    const float t = saturate(dot(p - a.xyz, ab) / max(dot(ab, ab), 1e-4f));
    return length(p - (a.xyz + ab * t)) - lerp(a.w, b.w, t);
}

// Mask 5's billow direction in the bind pose: horizontal, away from the wearer's upright axis, fading out within 4 units of it.
float3 ClothOutwardBind(float3 bindP) {
    const float2 o = bindP.xy - ClothLimbGround.zw;
    const float l = length(o);
    return float3(o / max(l, 1e-3f) * saturate(l * 0.25f), 0.0f);
}

// bindP: the vertex before skinning (modes 3 and 4; for unskinned meshes it is p itself). weight5: mask 5's per-vertex weight.
float ClothMask(float4 vertexColor, float3 p, float3 bindP, float weight5) {
    float m;
    [branch] if (ClothA.y > 4.5f) m = saturate(weight5);	// worn clothing (mask 5, core/ClothWeights.h) or still cloth (mask 6, core/ClothStatic.h) by its own mesh, vertex stream 1
    else [branch] if (ClothA.y > 3.5f) {	// worn clothing by its own skeleton (core/ClothWind.h SkeletonOf): only what stands off the legs
                                    // and forearms moves, only below the hips, nothing at the ground -- legs, trousers, boots, hands
                                    // and torso stay put
        float d = ClothLimbDistance(bindP, ClothLimb[0], ClothLimb[1]);
        d = min(d, ClothLimbDistance(bindP, ClothLimb[1], ClothLimb[2]));
        d = min(d, ClothLimbDistance(bindP, ClothLimb[2], ClothLimb[3]));
        d = min(d, ClothLimbDistance(bindP, ClothLimb[4], ClothLimb[5]));
        d = min(d, ClothLimbDistance(bindP, ClothLimb[5], ClothLimb[6]));
        d = min(d, ClothLimbDistance(bindP, ClothLimb[6], ClothLimb[7]));
        d = min(d, ClothLimbDistance(bindP, ClothLimb[8], ClothLimb[9]));
        d = min(d, ClothLimbDistance(bindP, ClothLimb[10], ClothLimb[11]));
        const float off = saturate((d - ClothLimbParams.z) * ClothLimbParams.w);
        const float h = saturate((ClothLimbParams.x - bindP.z) * ClothLimbParams.y);
        const float g = saturate((bindP.z - ClothLimbGround.x) * ClothLimbGround.y);
        m = off * off * (3.0f - 2.0f * off) * h * h * (3.0f - 2.0f * h) * g * g * (3.0f - 2.0f * g);
    }
    else [flatten] if (ClothA.y > 2.5f) {	// worn clothing: smooth from pinned (top part) to loose (hem)
        m = saturate((ClothD.z - bindP.z) * ClothD.w);
        m = m * m * (3.0f - 2.0f * m);
        const float cut = (ClothA.y - 3.0f) * 1000.0f;	// legs and boots (meshes with the feet at z 0): fades to 0 below this height, full 10 units above
        [flatten] if (cut > 0.5f) m *= saturate((bindP.z - cut) * 0.1f);
    }
    else m = ClothA.y > 1.5f ? saturate((ClothD.z - p.z) * ClothD.w) : (ClothA.y > 0.5f ? 1.0f : saturate(vertexColor.r));
    return ClothA.x * m;
}

// Moves the vertex and rebuilds tangent, binormal and normal. Every pass of an object must call it with the same inputs.
// weight5 / out5: mask 5's weight and billow direction; legL5 / legR5 / up5: its legs (ClothLegBind) and the bind pose's up, all carried
// by the bones (skinned space; 0 for everything else). Still meshes pass their stream 1 here too: weight5 = mask 6's weight, out5.x =
// its billow side.
void ClothWind(inout float3 p, inout float3 T, inout float3 B, inout float3 N, float4 vertexColor, float3 bindP, float weight5, float3 out5,
               float4 legL5, float4 legR5, float3 up5) {
    [branch] if (ClothA.x > 0.0f) {
        const float mask = ClothMask(vertexColor, p, bindP, weight5);
        const float e = 2.0f;
        const float3 n = normalize(N);
        const float3 bill = ClothBillowDir(n, out5);	// the billow's direction
        const float3 motion = ClothMotion(bindP);
        const float3 in5 = ClothInDir(out5);
        const float3 d0 = mask * ClothLimit5(ClothDisplacement(p, bill) + motion, in5, legL5, legR5, up5, mask);
        const float3 dt = mask * ClothLimit5(ClothDisplacement(p + T * e, bill) + motion, in5, legL5, legR5, up5, mask);
        const float3 db = mask * ClothLimit5(ClothDisplacement(p + B * e, bill) + motion, in5, legL5, legR5, up5, mask);
        const float3 t2 = T * e + (dt - d0);
        const float3 b2 = B * e + (db - d0);
        // The turn of the frame: the moved frame's normal minus the unmoved one's, added to the mesh's own normal -- so with no
        // displacement every vector comes back exactly as it was (the game's tangent frames are not always square).
        float3 n0 = normalize(cross(T, B));
        float3 n2 = normalize(cross(t2, b2));
        n0 = dot(n0, N) < 0.0f ? -n0 : n0;
        n2 = dot(n2, N) < 0.0f ? -n2 : n2;
        p += d0;
        T = normalize(t2) * length(T);
        B = normalize(b2) * length(B);
        N = normalize(n + (n2 - n0)) * length(N);
    }
}

// Shadow maps: position only, but the same displacement as the lit passes (2026-10-17: it used n = 0 and so left out the part along
// the normal -- up to ~0.85 of the intensity -- and the shadow caster sat off the cloth, which then shadowed itself in patches).
// The normal is in the same space as p and need not be unit (a missing one reads as zero: the push along the wind only).
float3 ClothWindPosition(float3 p, float3 n, float4 vertexColor, float3 bindP, float weight5, float3 out5, float4 legL5, float4 legR5, float3 up5) {
    [branch] if (ClothA.x > 0.0f) {
        const float l = length(n);
        const float3 bill = ClothBillowDir(l > 1e-4f ? n / l : 0.0f, out5);
        const float mask = ClothMask(vertexColor, p, bindP, weight5);
        p += mask * ClothLimit5(ClothDisplacement(p, bill) + ClothMotion(bindP), ClothInDir(out5), legL5, legR5, up5, mask);
    }
    return p;
}

// ObjectTemplate: the tangent frame is a float3x3 (rows tangent, binormal, normal).
void ClothWindTBN(inout float3 p, inout float3x3 tbn, float4 vertexColor, float3 bindP, float weight5, float3 out5, float4 legL5, float4 legR5, float3 up5) {
    float3 T = tbn[0], B = tbn[1], N = tbn[2];
    ClothWind(p, T, B, N, vertexColor, bindP, weight5, out5, legL5, legR5, up5);
    tbn = float3x3(T, B, N);
}
