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
// The dry layers (mud, sand, frost) lie in blotches of GearWeatherScale units (GWField). The wet layer does not (2026-11-26 rain fix): it is
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
// The two fields the dry layers read (mud, sand, frost): x coarse (blotches ~1 / scale units across), y fine (a quarter of that).
float2 GWField(float3 local) {
    const float3 q = local * GW_Look.x + GW_Look.w;
    return float2(GWNoise(q), GWNoise(q * 4.03f + 17.0f));
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
    // blended by distance
    float2 field;
    float4 bead;
    [branch] if (GW_Part.x < 0.5f) {
        const float3 l = float3(dot(GW_Rigid[0], P1), dot(GW_Rigid[1], P1), dot(GW_Rigid[2], P1));
        field = GWField(l);
        bead = GWBead(l, GW_Rigid[0].xyz, GW_Rigid[1].xyz, GW_Rigid[2].xyz, beadShare);
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
        const float2 f0 = GWField(l0);
        const float2 f1 = GWField(l1);
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

    // mud: splashed up from the ground (the lower and side faces), dark, dull, it hides the metal and fills the grooves
    const float mud = dry * saturate((GW_Level.w * 1.7f - 0.35f) + (field.x - 0.5f) * 1.3f) * saturate(0.75f - 0.45f * N.z);
    a = lerp(a, float3(0.105f, 0.078f, 0.052f), mud * 0.9f);
    rough = lerp(rough, 0.9f, mud);
    metal *= 1.0f - mud;
    N = normalize(lerp(N, Ns, mud * 0.7f));

    // sand and dust: settles on the faces turned up and in the crevices, pale and matte
    const float sand = dry * saturate(GW_Level.z * 1.6f * (0.3f + 0.7f * max(up, crevice)) + (field.x * 0.6f + field.y * 0.4f - 0.5f) * 0.9f - 0.12f);
    a = lerp(a, float3(0.40f, 0.31f, 0.20f), sand * 0.85f);
    rough = lerp(rough, 0.92f, sand);
    metal *= 1.0f - sand * 0.9f;
    N = normalize(lerp(N, Ns, sand * 0.5f));

    // frost: the rims and the metal first (thin, cold parts), pale and rough, its crystals tilting the normal a little
    const float frost = dry * saturate(GW_Level.y * 1.5f * (0.4f + 0.35f * edge + 0.45f * metal) + (field.y - 0.5f) * 0.9f - 0.15f);
    a = lerp(a, float3(0.62f, 0.68f, 0.74f), frost * 0.8f);
    rough = lerp(rough, 0.65f, frost);
    metal *= 1.0f - frost * 0.7f;
    N = normalize(N + (float3(field.y, frac(field.y * 7.13f), frac(field.y * 3.71f)) - 0.5f) * (0.35f * frost));

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

    // DebugView ([Main.Main.Quality] GearWeatherDebug 1): the four covers as colours over the albedo (r mud, g sand, b frost, grey wet, the
    // beads white)
    [branch] if (GW_Look.z > 0.5f) a = saturate(float3(mud, sand, frost) * 0.8f + wet * 0.4f + beadCover * 0.5f + 0.1f);

    PS_OUT o;
    const float2 e = hasN ? GWEncode(N) : m.ga;	// a record without a shading normal keeps "none" (the passes take the smooth normal)
    // no cover at all here: both texels exactly as the record wrote them (its roughness codes, its normal)
    const bool covered = mud + sand + frost + wet > 1.0e-4f || GW_Look.z > 0.5f;
    o.albedo = covered ? float4(a, albedo.a) : albedo;
    o.material = covered ? float4(model + specular + 0.49f * saturate(metal), e.x, saturate(rough), e.y) : m;
    return o;
}
