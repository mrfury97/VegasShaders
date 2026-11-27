// UNOFFICIAL 2026-11-15 GearWeather (core/GearWeather.h; docs/next/FD-FIRSTPERSON-RESULTS.md): weather on the player's gear, first person,
// full deferred renderer. One small pass per first-person draw that wrote the G-buffer (DeferredFirstPerson's span), over that draw's own
// pixels only (the stencil holds the draw's id, core/GearWeather.h DrawBegin), after the G-buffer is made and before the light passes. It
// changes the surface the light passes then light: the albedo (COLOR1 rgb), the roughness, the metal and the shading normal (COLOR2), so
// the sun, every lamp and the shadows shade wet, frosty, sandy or muddy gear as they shade any surface. Nothing is lit here.
//
// The pattern is fixed to the gear, not to the screen: the pixel's position (from COLOR3's camera distance along the first-person
// camera's ray) is taken into the draw's own space (a still part: the part's world transform; a skinned part, the hands and sleeves: its
// two nearest bones, blended), and the noise is read there. A reload or a walk moves the gun and the hands; the frost on them moves with
// them. The two G-buffer targets are written whole from copies of them (s0 / s1, made by GearWeather::Apply over the view model's box): a
// render target is never read while it is bound.
//
// The dry layers (mud, sand, frost; 2026-11-27 upstream 1.1.5: splatters, grains, ice and snowflakes) lie in blotches of GearWeatherScale
// units (GWField) and finer cells. The wet layer does not (2026-11-26 rain fix): it is
// one even film, darker and a little smoother (roughness down to 0.28, never a mirror), with small sparse beads of a fixed size (GWBead, a
// few tenths of a unit, whatever the scale) that are smoother still and bulge the normal. In rain the C++ rinses the dry layers off in
// about 10 s, so rain shows the wet layer alone.
//
// Not compiled into any template, defines nothing for them: the templates are exactly as before. Loaded at startup by GearWeather::Load
// (ShaderRecord::LoadShader, never on the render thread) only while [Main.Main.Quality] DeferredFirstPerson and GearWeather are on.

#include "Includes/DeferredDepth.hlsl"

float4 GW_Cam[6]  : register(c0);	// the first-person camera's frame in Includes/DeferredDepth.hlsl's DD layout (axes, tan, viewport, shift)
float4 GW_Level   : register(c6);	// x wet, y frost, z sand / dust, w mud: each layer's amount now (built up / faded on the CPU), x its strength
float4 GW_Look    : register(c7);	// x the noise's scale (1 / game units), y the bare skin's share of the dry layers, z DebugView, w the draw's seed
float4 GW_Anim    : register(c76);	// x real-time clock (s), y active snowfall (0-1), z active sandstorm (0-1), w unused
float4 GW_Part    : register(c8);	// x the bones (0: a still part, its rows in GW_Rigid), y 1 = a weapon part
float4 GW_Rigid[3] : register(c9);	// the still part: gear-local = (dot(r0, P1), dot(r1, P1), dot(r2, P1)), P1 = (camera-relative world position, 1)
float4 GW_BoneS[16] : register(c12);	// up to 16 bones (a skinned part): the bone's bound (centre camera-relative, radius) ...
float4 GW_BoneX[16] : register(c28);	// ... and its three rows as GW_Rigid's (one array a row: the loop indexes them by its own counter)
float4 GW_BoneY[16] : register(c44);
float4 GW_BoneZ[16] : register(c60);

sampler2D GW_Albedo   : register(s0);	// a copy of COLOR1 (TESR_AlbedoBuffer): rgb linear albedo, a the deferred code (kept)
sampler2D GW_Material : register(s1);	// a copy of COLOR2 (TESR_MaterialBuffer, GBufferV2): r model (+16 specular) + 0.49 metal, g / a the shading normal, b roughness
sampler2D GW_Normals  : register(s2);	// COLOR3 (TESR_CapturedNormalsBuffer): rg the smooth normal, b the camera distance, a the actor / skin code

struct PS_IN { float2 uv : TEXCOORD0; float2 vpos : VPOS; };
struct PS_OUT { float4 albedo : COLOR0; float4 material : COLOR1; };

float3 GWDecode(float2 e) {
    float3 n = float3(e, 1.0f - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0f ? -t : t, n.y >= 0.0f ? -t : t);
    return normalize(n);
}
float2 GWEncode(float3 n) {
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1.0e-6f);
    float2 e = n.xy;
    if (n.z < 0.0f) e = (1.0f - abs(n.yx)) * float2(n.x >= 0.0f ? 1.0f : -1.0f, n.y >= 0.0f ? 1.0f : -1.0f);
    return e;
}

// A hash of a lattice point to 0..1, and smooth value noise between the eight around a point (written for this pass).
float GWHash(float3 p) {
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}
float3 GWHash3(float3 p) {
    p = frac(p * float3(0.1031f, 0.1030f, 0.0973f));
    p += dot(p, p.yxz + 33.33f);
    return frac((p.xxy + p.yxx) * p.zyx);
}
float GWNoise(float3 x) {
    const float3 i = floor(x);
    float3 f = x - i;
    f = f * f * (3.0f - 2.0f * f);
    const float a = lerp(GWHash(i), GWHash(i + float3(1, 0, 0)), f.x);
    const float b = lerp(GWHash(i + float3(0, 1, 0)), GWHash(i + float3(1, 1, 0)), f.x);
    const float c = lerp(GWHash(i + float3(0, 0, 1)), GWHash(i + float3(1, 0, 1)), f.x);
    const float d = lerp(GWHash(i + float3(0, 1, 1)), GWHash(i + float3(1, 1, 1)), f.x);
    return lerp(lerp(a, b, f.y), lerp(c, d, f.y), f.z);
}
// The field the dry layers read (mud, sand, frost; the wet film and its beads do not): x medium-coarse (~1 / scale), y fine breakup,
// z crisp surface noise. (2026-11-27 upstream 1.1.5 merge: upstream's three octaves.)
float3 GWField(float3 local) {
    const float3 q = local * GW_Look.x + GW_Look.w;
    return float3(GWNoise(q * 1.15f), GWNoise(q * 4.7f + 17.3f), GWNoise(q * 15.5f + 41.7f));
}
// One staggered 3D cell layer of landing & melting snowflakes (HudRainLayer-style shrink over life).
// Returns xy: crystal dome tilt, z: flake mask (0..1).
float3 GWFlakeLayer(float3 p, float t, float speed, float supply, float breakup) {
    const float3 ci = floor(p);
    const float3 cf = p - ci;
    const float3 h = GWHash3(ci);
    const float3 c = 0.22f + 0.56f * h;
    const float life = 1.0f - frac(h.z + t * speed);
    const float alive = saturate(life * (0.45f + 0.55f * h.x) - lerp(0.97f, 0.34f, supply));
    const float r = (0.09f + 0.08f * h.y) * (0.78f + 0.44f * breakup) * saturate(alive * 2.4f);
    const float3 delta = cf - c;
    const float d = length(delta);
    const float edgeW = max(r * 0.32f, 0.012f);
    const float mask = smoothstep(r, max(r - edgeW, 0.0f), d) * step(0.01f, alive);
    return float3((delta.xy / max(r, 0.01f)) * mask, mask);
}
// One 3D cell layer of sandstorm grains: x mask, y shade (-0.5..0.5), z quartz glint mask, w slope x.
float4 GWSandGrainLayer(float3 p, float threshold) {
    const float3 ci = floor(p);
    const float3 cf = p - ci;
    const float3 h = GWHash3(ci);
    const float3 c = 0.22f + 0.56f * h;
    const float present = step(threshold, h.x);
    const float r = lerp(0.11f, 0.20f, h.y) * present;
    const float3 delta = cf - c;
    const float d = length(delta);
    const float mask = smoothstep(r, max(r * 0.45f, 0.01f), d) * present;
    const float glint = step(0.91f, h.z) * mask;
    return float4(mask, h.z - 0.5f, glint, (delta.x / max(r, 0.01f)) * mask);
}
// One 3D cell layer of irregular organic mud spatters: xy raised droplet slope, z mask (0..1).
float3 GWMudSplatLayer(float3 p, float threshold, float warp) {
    const float3 ci = floor(p);
    const float3 cf = p - ci;
    const float3 h = GWHash3(ci);
    const float present = step(threshold, h.x);
    const float3 c = 0.22f + 0.56f * h;
    const float3 delta = cf - c;
    const float r = lerp(0.10f, 0.24f, h.y) * (0.72f + 0.56f * warp) * present;
    const float d = length(delta * float3(1.0f, lerp(0.75f, 1.35f, h.z), 1.0f));
    const float edgeW = max(r * 0.28f, 0.012f);
    const float mask = smoothstep(r, max(r - edgeW, 0.0f), d) * present;
    return float3((delta.xy / max(r, 0.01f)) * mask, mask);
}

// UNOFFICIAL 2026-11-26 rain beads: the wet layer's drops, a pattern of their own fixed to the part, the same size whatever GearWeatherScale.
// One drop at most per lattice cell of GW_BEAD_CELL units (in a cell with probability `share`), its middle and radius picked by the cell's
// hash and kept inside the cell (middle 0.3-0.7, radius 0.12-0.27 of a cell: 0.1-0.22 units across), so no neighbour cell is read.
// r0..r2: the part's rows (GW_Rigid or the bone's): their transpose takes a local direction to world axes. Returns xyz the direction from
// the drop's middle to the point in world axes, its length the distance over the radius (0 at the middle, 1 at the rim), and w the drop's
// cover (1 inside, falling to 0 over the outer fifth).
#define GW_BEAD_CELL 0.4f
float4 GWBead(float3 local, float3 r0, float3 r1, float3 r2, float share) {
    const float3 q = local * (1.0f / GW_BEAD_CELL) + GW_Look.w * 1.37f;
    const float3 i = floor(q);
    const float3 c = 0.3f + 0.4f * float3(GWHash(i + 7.1f), GWHash(i + 19.3f), GWHash(i + 29.9f));
    const float3 d = (q - i - c) / lerp(0.12f, 0.27f, GWHash(i + 53.5f));
    const float radial = length(d);
    const float cover = GWHash(i + 41.7f) < share ? saturate((1.0f - radial) * 5.0f) : 0.0f;
    const float3 w = d.x * r0 + d.y * r1 + d.z * r2;
    return float4(w * (radial * rsqrt(max(dot(w, w), 1.0e-12f))), cover);
}

PS_OUT main(PS_IN IN) {
    const float4 uv4 = float4(IN.uv, 0.0f, 0.0f);
    const float4 albedo = tex2Dlod(GW_Albedo, uv4);
    const float4 m = tex2Dlod(GW_Material, uv4);
    const float4 normals = tex2Dlod(GW_Normals, uv4);
    clip(albedo.a - 1.0001f);	// a deferred record's pixel (forward records write 1: their light is their own)
    clip(m.r - 0.5f);			// written
    float model = floor(m.r + 0.01f);
    const float specular = model > 15.5f ? 16.0f : 0.0f;
    model -= specular;
    clip(model < 8.5f ? 1.0f : -1.0f);	// not eyes (9), not unlit (10)
    float metal = saturate((m.r - floor(m.r + 0.004f)) / 0.49f);
    const bool hasN = abs(m.g) <= 1.001f;
    const float3 Ns = GWDecode(normals.rg);
    float3 N = hasN ? GWDecode(m.ga) : Ns;
    float rough = m.b < -1.5f ? -2.0f - m.b : m.b < 0.0f ? 1.0f : m.b;

    // the point, camera-relative (world axes, the first-person camera at the origin), and the view direction
    const float3 ray = DD_CAMERA_RELATIVE_Z(GW_Cam, IN.vpos, 1.0f);
    const float3 P = ray * (normals.b * rsqrt(dot(ray, ray)));
    const float3 V = -normalize(P);
    const float4 P1 = float4(P, 1.0f);

    const bool skin = abs(model - 6.0f) < 0.5f;
    const float wet = saturate(GW_Level.x);
    const float beadShare = wet * (skin ? 0.3f : 0.6f);	// (2026-11-26) the share of bead cells holding a drop: more drops as the gear gets wetter

    // the field and the beads in the part's own space: a still part through its transform; a skinned one through its two nearest bones,
    // blended by distance. (2026-11-27 upstream 1.1.5 merge: upstream's float3 field and localPos, which its grains and flakes read; our beads.)
    float3 field;
    float3 localPos;
    float4 bead;
    [branch] if (GW_Part.x < 0.5f) {
        localPos = float3(dot(GW_Rigid[0], P1), dot(GW_Rigid[1], P1), dot(GW_Rigid[2], P1));
        field = GWField(localPos);
        bead = GWBead(localPos, GW_Rigid[0].xyz, GW_Rigid[1].xyz, GW_Rigid[2].xyz, beadShare);
    }
    else {
        float d0 = 1.0e9f, d1 = 1.0e9f;
        float3 l0 = 0.0f, l1 = 0.0f;
        float3 x0 = 0.0f, y0 = 0.0f, z0 = 0.0f;	// the nearest bone's rows (the beads' directions)
        [loop] for (int b = 0; b < 16; b++) {
            [branch] if ((float)b >= GW_Part.x) break;
            const float d = max(length(P - GW_BoneS[b].xyz) - GW_BoneS[b].w, 0.0f);
            const float3 l = float3(dot(GW_BoneX[b], P1), dot(GW_BoneY[b], P1), dot(GW_BoneZ[b], P1));
            [flatten] if (d < d0) { d1 = d0; l1 = l0; d0 = d; l0 = l; x0 = GW_BoneX[b].xyz; y0 = GW_BoneY[b].xyz; z0 = GW_BoneZ[b].xyz; }
            else if (d < d1) { d1 = d; l1 = l; }
        }
        localPos = l0;
        const float3 f0 = GWField(l0);
        const float3 f1 = GWField(l1);
        const float w1 = d1 > 1.0e8f ? 0.0f : saturate(0.5f - (d1 - d0) / max(2.0f * (d0 + d1), 0.5f));	// 0.5 at the halfway, 0 well inside one bone
        field = lerp(f0, f1, w1);
        // the beads from the nearest bone alone: a drop is never blended into a ghost of the next bone's (one cut at the halfway between two
        // bones instead, rare at a drop's size)
        bead = GWBead(l0, x0, y0, z0, beadShare);
    }

    const float dry = skin ? GW_Look.y : 1.0f;	// bare skin takes the dry layers weaker (it wipes off, it sweats)
    const float up = saturate(N.z * 1.25f + 0.15f);						// faces turned to the sky (world z up)
    const float crevice = saturate((1.0f - dot(N, Ns)) * 6.0f);		// where the normal map bends the surface away: seams, grooves, knurling
    const float edge = 1.0f - saturate(dot(Ns, V));						// the part's rims as the eye sees them
    float3 a = albedo.rgb;

    // (2026-11-27 upstream 1.1.5 merge: upstream's dry layers below, mud splatters, sand dust with grains, ice with rime and falling and
    // melting snowflakes, each gated by its level (or the live snowfall / sandstorm, GW_Anim); our wet film and beads after them.)
    // mud: rich dark earthy splatters and fine droplets instead of blocky camo patches; underside/crevice smears + upper flung drops
    float mud = 0.0f;
    [branch] if (GW_Level.w > 0.001f) {
        const float3 mp = float3(localPos.x * 0.8f - localPos.y * 0.6f, localPos.x * 0.6f + localPos.y * 0.8f, localPos.z);
        const float mudZone = saturate(max(saturate(0.42f - 0.62f * N.z), crevice * 0.85f));
        const float mudSmear = smoothstep(0.18f, 0.42f, GW_Level.w * 0.90f * mudZone + (field.x * 0.55f + field.y * 0.45f - 0.56f) + (field.z - 0.5f) * 0.22f);
        const float3 sp0 = GWMudSplatLayer(mp * 1.9f + GW_Look.w, lerp(0.96f, 0.54f, GW_Level.w), field.z);
        const float3 sp1 = GWMudSplatLayer(mp * 4.4f + GW_Look.w * 1.9f + 13.7f, lerp(0.95f, 0.42f, GW_Level.w), field.y);
        const float splatAllow = saturate(0.55f - 0.25f * N.z);
        const float mudSplat = saturate(max(sp0.z, sp1.z * 0.9f) * splatAllow);
        // (2026-11-27 upstream 1.1.5 merge: x saturate(level x 4) as our 2026-11-26 rain fix: while the rain rinses it off, mud thins away
        // instead of holding full blotches until the level falls under the gate)
        const float mudFade = saturate(GW_Level.w * 4.0f);
        mud = dry * saturate(max(mudSmear, mudSplat)) * mudFade;
        const float3 mudCol = lerp(float3(0.038f, 0.026f, 0.016f), float3(0.075f, 0.052f, 0.032f), field.z);
        a = lerp(a, mudCol, mud * 0.92f);
        rough = lerp(rough, lerp(0.84f, 0.44f, saturate(mudSplat * 1.2f)), mud);
        metal *= 1.0f - mud * 0.94f;
        N = normalize(lerp(N, Ns, mud * 0.50f) + float3((sp0.x + sp1.x * 0.8f) * 0.32f * splatAllow * mudFade + (field.z - 0.5f) * 0.16f * mud, (sp0.y + sp1.y * 0.8f) * 0.32f * splatAllow * mudFade + (field.y - 0.5f) * 0.16f * mud, 0.0f));
    }

    // sand and dust: wind-swept patchy desert dust keeping open metal flats clean + crisp 3D grain scales + 52x silica micro-grit
    float sand = 0.0f;
    float sandGrains = 0.0f;
    [branch] if (GW_Level.z > 0.001f || GW_Anim.z > 0.001f) {
        const float sandLevel = max(GW_Level.z, GW_Anim.z * 0.45f);
        const float sandCarrier = smoothstep(0.36f, 0.68f, field.x * 0.62f + field.y * 0.38f + (field.z - 0.5f) * 0.22f + crevice * 0.28f - 0.06f);
        const float dustSeat = max(up * 0.85f, crevice * 1.15f);
        const float dustNoise = field.x * 0.40f + field.y * 0.38f + field.z * 0.35f;
        const float dustField = sandLevel * (0.25f + 0.85f * dustSeat) + (dustNoise - 0.58f) * 1.05f - 0.30f;
        const float sandFade = saturate(sandLevel * 4.0f);	// (2026-11-27 merge: as mudFade; a storm alone brings sandLevel up too)
        sand = dry * saturate(smoothstep(0.05f, 0.45f, dustField) * lerp(0.55f, 1.0f, field.z) * sandCarrier) * sandFade;
        const float3 dustCol = lerp(float3(0.32f, 0.24f, 0.16f), float3(0.46f, 0.37f, 0.25f), field.z);
        a = lerp(a, dustCol, sand * 0.60f);
        rough = lerp(rough, 0.90f, sand * 0.78f);
        metal *= 1.0f - sand * 0.68f;
        N = normalize(lerp(N, Ns, sand * 0.45f));

        const float grainAct = saturate(sandLevel * 1.35f);
        const float4 g0 = GWSandGrainLayer(localPos * 12.0f + GW_Look.w, lerp(0.95f, 0.42f, grainAct));
        const float4 g1 = GWSandGrainLayer(localPos * 26.0f + GW_Look.w * 2.3f + 7.1f, lerp(0.92f, 0.30f, grainAct));
        const float3 h52 = GWHash3(floor(localPos * 52.0f + float3(field.y, field.z, field.x) * 1.3f + GW_Look.w));
        const float microDust = step(lerp(0.96f, 0.62f, grainAct), h52.x);
        const float microQuartz = step(0.92f, h52.y) * microDust;
        const float grainSurface = saturate(0.28f + 0.72f * max(up, crevice)) * lerp(0.18f, 1.0f, sandCarrier) * dry * sandFade;
        sandGrains = saturate(max(max(g0.x, g1.x * 0.85f), microDust * 0.75f) * grainSurface);
        const float grainShade = g0.y * 0.10f + g1.y * 0.08f + (h52.z - 0.5f) * 0.14f * microDust;
        const float grainGlint = saturate(max(max(g0.z, g1.z), microQuartz) * grainSurface);
        a = lerp(a, saturate(float3(0.45f, 0.35f, 0.23f) + grainShade), sandGrains * 0.92f);
        metal *= 1.0f - sandGrains * 0.92f;
        rough = lerp(rough, lerp(0.92f, 0.18f, grainGlint), sandGrains);
        N = normalize(N + float3(g0.w * 0.24f + g1.w * 0.20f + (h52.y - 0.5f) * 0.28f * microDust, (field.z - 0.5f) * 0.25f * sandGrains, 0.0f) * sandFade);
    }

    // frost & ice: clear glass-like ice centers showing dark gunmetal through, milky frosted patch rims & grazing Fresnel, strict crevice rime + flakes
    float frost = 0.0f;
    float flakes = 0.0f;
    [branch] if (GW_Level.y > 0.001f || GW_Anim.y > 0.001f) {
        const float iceCrystal0 = GWNoise(localPos * 14.0f + GW_Look.w);
        const float iceCrystal1 = GWNoise(localPos * 30.0f + float3(iceCrystal0, field.y, field.z) * 1.4f + 9.2f);
        const float vein = 1.0f - abs(iceCrystal0 * 2.0f - 1.0f);
        const float frostBias = 0.25f + 0.30f * edge + 0.30f * metal + 0.25f * crevice + 0.15f * up;
        const float iceWarp = (field.y - 0.5f) * 0.30f + (iceCrystal0 - 0.5f) * 0.22f + (iceCrystal1 - 0.5f) * 0.14f;
        const float iceThresh = lerp(0.86f, 0.46f, saturate(GW_Level.y * (0.45f + 0.65f * frostBias)));
        const float frostCore = (field.x + iceWarp) - iceThresh;
        const float iceGlaze = dry * smoothstep(0.0f, 0.12f, GW_Level.y) * smoothstep(0.01f, 0.15f, frostCore);

        const float patchRim = smoothstep(0.0f, 0.045f, frostCore) * (1.0f - smoothstep(0.045f, 0.13f, frostCore));
        const float vein2 = vein * vein;
        const float iceCloud = saturate(patchRim * 0.75f + vein2 * vein * 0.45f + iceCrystal1 * 0.20f);

        const float3 facetGrad = float3(iceCrystal0 - 0.5f, iceCrystal1 - 0.5f, (field.z - 0.5f) * 0.5f);
        const float3 iceN = normalize(N + facetGrad * (0.42f * iceGlaze));
        N = iceN;
        const float iceFresnel = pow(1.0f - saturate(dot(iceN, V)), 3.6f);
        const float iceTint = iceGlaze * saturate(lerp(0.05f, 0.34f, iceCloud) + iceFresnel * 0.38f);
        const float3 iceCol = lerp(float3(0.62f, 0.76f, 0.88f), float3(0.86f, 0.93f, 0.98f), saturate(iceCloud * 0.5f + iceFresnel));
        a = lerp(a, iceCol, iceTint);
        const float iceRough = lerp(0.06f, 0.24f, iceCloud);
        rough = lerp(rough, iceRough, iceGlaze);
        metal *= 1.0f - iceGlaze * lerp(0.45f, 0.85f, saturate(iceCloud + iceFresnel));

        const float rimeCrevice = smoothstep(0.18f, 0.68f, crevice);
        const float rimeNoise = smoothstep(0.42f, 0.74f, iceCrystal0 * 0.55f + field.z * 0.45f);
        const float rime = dry * saturate(GW_Level.y * 1.25f) * rimeCrevice * rimeNoise;
        a = lerp(a, float3(0.82f, 0.88f, 0.94f), rime * 0.62f);
        rough = lerp(rough, 0.62f, rime * 0.85f);
        metal *= 1.0f - rime * 0.85f;

        const float sparkH = GWHash(floor(localPos * 42.0f + GW_Look.w));
        const float microSparkle = step(0.955f, sparkH) * saturate(iceGlaze + rime);
        a = lerp(a, float3(0.94f, 0.97f, 1.0f), microSparkle * 0.75f);
        rough = lerp(rough, 0.08f, microSparkle);

        frost = saturate(max(iceGlaze, rime));

        const float flakeSupply = saturate(max(GW_Anim.y, GW_Level.y * 0.65f));
        [branch] if (flakeSupply > 0.01f) {
            const float flakeSurface = saturate(N.z * 1.35f + 0.35f) * dry;
            const float3 fl0 = GWFlakeLayer(localPos * 3.2f + GW_Look.w, GW_Anim.x, 0.085f, flakeSupply, field.z);
            const float3 fl1 = GWFlakeLayer(localPos * 5.8f + GW_Look.w * 1.7f + 19.3f, GW_Anim.x, 0.12f, flakeSupply, field.y);
            const float3 fl = (fl0 + fl1 * 0.85f) * flakeSurface;
            flakes = saturate(fl.z);
            a = lerp(a, float3(0.86f, 0.90f, 0.95f), flakes * 0.88f);
            metal *= 1.0f - flakes;
            const float melting = saturate((0.15f - GW_Anim.y) / 0.15f) * saturate(GW_Level.y * 4.0f);
            rough = lerp(rough, lerp(0.32f, 0.18f, melting), flakes);
            N = normalize(N + float3(fl.xy * 0.28f, 0.0f));
        }
    }

    // wet (2026-11-26 rain fix): an even film over the whole part, no blotches. The colour darkens (porous x 0.55, metal x 0.95: never above
    // the dry albedo) and the surface gets a little smoother, down to roughness 0.28 (skin 0.3) and never rougher than it was: a soft sheen
    // from the sun and the lamps, not a mirror. (It was 0.07 under a 3-unit noise: the light passes gave mirror glints where the noise was
    // wet and dull patches between, which read as big pale camo blotches.) The beads (GWBead) are smoother still (0.1) and bulge the normal
    // outward from their middles: small sparse glints. The record's ambient and sky light (COLOR0, written before this pass) are not
    // touched, so under sky light wet gear is exactly as bright as dry; the sun's and the lamps' diffuse only lose light (darker albedo,
    // a smaller diffuse keep and more Fresnel at the lower roughness), and their highlight only gathers the same light into a tighter lobe.
    a *= lerp(1.0f, lerp(0.55f, 0.95f, metal), wet * (skin ? 0.4f : 1.0f));
    rough = lerp(rough, min(rough, skin ? 0.3f : 0.28f), wet * (skin ? 0.85f : 1.0f));
    N = normalize(lerp(N, Ns, wet * 0.35f));
    const float beadCover = bead.w * saturate(wet * 2.0f);
    rough = lerp(rough, min(rough, 0.1f), beadCover);
    N = normalize(N + (bead.xyz - N * dot(bead.xyz, N)) * (0.45f * beadCover));

    // DebugView ([Main.Main.Quality] GearWeatherDebug 1): the covers as colours over the albedo (r mud, g sand and grains, b frost and flakes,
    // grey wet, the beads white). (2026-11-27 upstream 1.1.5 merge: upstream's layers, our wet and beads.)
    [branch] if (GW_Look.z > 0.5f) a = saturate(float3(mud + saturate(sand + sandGrains) * 0.3f, saturate(sand + sandGrains), saturate(frost + flakes)) * 0.8f + wet * 0.4f + beadCover * 0.5f + 0.1f);

    PS_OUT o;
    const float2 e = hasN ? GWEncode(N) : m.ga;	// a record without a shading normal keeps "none" (the passes take the smooth normal)
    // no cover at all here: both texels exactly as the record wrote them (its roughness codes, its normal)
    const bool covered = mud + sand + sandGrains + frost + flakes + wet > 1.0e-4f || GW_Look.z > 0.5f;
    o.albedo = covered ? float4(a, albedo.a) : albedo;
    o.material = covered ? float4(model + specular + 0.49f * saturate(metal), e.x, saturate(rough), e.y) : m;
    return o;
}
