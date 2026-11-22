// UNOFFICIAL 2026-10-28 CapturedNormals (core/CapturedNormals.h, docs/next/CAPTURED-NORMALS.md; macintroll's proposal): while the
// world's main opaque list draws, NVR's lit templates (ObjectTemplate, ParallaxTemplate, SkinTemplate, TerrainTemplate) write the surface's
// smooth interpolated vertex normal (world space, after skinning, BEFORE the normal map) to a fourth render target, COLOR3:
//   rg  the normal, octahedral (Includes/Shadow.hlsl ShadowEncodeNormal), on the camera's side: of the view ray (CapturedNormalOut: the
//       land, skin, foliage), or (2026-11-16 fd-roofedge) of the triangle (CapturedNormalOutTri: objects and parallax)
//   b   the camera distance (length of the camera-relative world position), which readers compare against the depth buffer
//   a   1 = written by a template, 2 = by an actor's (bone-skinned draws: SKIN, or the skin template; 2026-10-28: snow keeps off actors)
//       (0 = cleared, or a variant with no smooth normal)
// The target is bound only inside that list and only for those templates' opaque draws (anything else masks it off, COLORWRITEENABLE3);
// anywhere else COLOR3 is discarded. Compiled in only with [Shaders.SnowAccumulation.Main] CapturedNormals on at startup
// (CAPTURED_NORMALS_ON, ShaderRecord::BuildDefines): off, the templates are exactly as before. Needs Includes/Shadow.hlsl first.
#ifndef NVR_CAPTURED_NORMALS_HLSL
#define NVR_CAPTURED_NORMALS_HLSL

#ifndef CAPTURED_NORMALS_ON
    #define CAPTURED_NORMALS_ON 0
#endif

#if CAPTURED_NORMALS_ON
    #define CAPTURED_NORMAL_INVALID float4(0.0f, 0.0f, 0.0f, 0.0f)
    // n: the smooth vertex normal (camera-relative world space, any length), worldPos: the camera-relative world position.
    float4 CapturedNormalOut(float3 n, float3 worldPos) {
        const float len2 = dot(n, n);
        [flatten] if (len2 < 1.0e-6f) return CAPTURED_NORMAL_INVALID;
        n *= rsqrt(len2);
        n *= dot(n, worldPos) > 0.0f ? -1.0f : 1.0f;	// the side the camera sees (two-sided cards, cloth)
        #if defined(SKIN) || defined(CAPTURED_ACTOR)
            return float4(ShadowEncodeNormal(n), length(worldPos), 2.0f);
        #else
            return float4(ShadowEncodeNormal(n), length(worldPos), 1.0f);
        #endif
    }

    // UNOFFICIAL 2026-11-16 fd-roofedge (docs/next/FD-ROOFEDGE.md): which side of the TRIANGLE the camera is on decides the flip, not the
    // smooth normal's angle to the view ray. A mesh shaded smooth across a sharp edge (a roof ridge: its vertices' normals averaged
    // straight up) seen from below that edge shows its front face while the interpolated normal points away from the camera; the view
    // test above turned it to point DOWN, and the deferred passes lit the band under the ridge with a normal facing away from the sun (a
    // dark wedge along the roofs' top edges under the deferred renderer; forward lights in the vertex tangent frame and never sees it).
    // p: the camera-relative world position (the camera is the origin); face: the triangle's normal as wound, any length. -1 only where
    // n lies behind the triangle as the camera sees it (two-sided cards and cloth seen from behind); where the triangle is edge-on (no
    // usable face) the view test, as before.
    float CapturedTriangleFlip(float3 n, float3 p, float3 face) {
        const float side = dot(face, p);	// < 0: the face as wound points at the camera
        const bool usable = side * side > 1.0e-8f * dot(face, face) * dot(p, p);
        return (usable ? dot(n, face) * side : dot(n, p)) > 0.0f ? -1.0f : 1.0f;
    }
    // CapturedNormalOut with the triangle's side (above). ddx / ddy: call at the pixel shader's top level only. The object and parallax
    // records; the land keeps CapturedNormalOut (a heightfield's normal does not point away from a camera that sees its top).
    float4 CapturedNormalOutTri(float3 n, float3 worldPos) {
        const float len2 = dot(n, n);
        const float flip = CapturedTriangleFlip(n, worldPos, cross(ddx(worldPos), ddy(worldPos)));
        [flatten] if (len2 < 1.0e-6f) return CAPTURED_NORMAL_INVALID;
        n *= rsqrt(len2) * flip;
        #if defined(SKIN) || defined(CAPTURED_ACTOR)
            return float4(ShadowEncodeNormal(n), length(worldPos), 2.0f);
        #else
            return float4(ShadowEncodeNormal(n), length(worldPos), 1.0f);
        #endif
    }

    // UNOFFICIAL 2026-10-29 MaterialBuffer (core/CapturedNormals.h; [Main.Main.Quality] MaterialBuffer, restart; CAPTURED_NORMALS_ON 2): the
    // templates' spare COLOR2 = TESR_MaterialBuffer while the main list draws: r the material class (Render.cpp per draw: 1 object, 2 worn
    // gear (skinned), 3 cloth, 4 land, 5 plants), g the drawn object's world bound radius (thickness, an upper bound), b the roughness the
    // shader lit with (-1: none), a 1. Actors' skin is not in it (its template's COLOR2 is the skin blur's): the normals' a == 2 marks it.
    // 2026-10-31: r = class + 0.49 x metal (the share of the pixel lit as metal, [Shaders.PBR.Metal]): class = floor(r), metal =
    // frac(r) / 0.49. Readers that test r > 0.5 for "written" stay right. Skin is filled in after the list (class 6, core/CapturedNormals.h).
    #if CAPTURED_NORMALS_ON >= 2
        // 2026-10-31: the draw's class and bound radius ride in NVR_SpecFade.w (c211, Includes/PBR.hlsl: class + 8 x floor(radius), set per draw
        // by Render.cpp MaterialForPass through core/PbrPop.h). They were on c217 until then, which the RT lamp shadows' NVR_PointShadowRTSlot[2]
        // (c216-c217) also holds: every interior lamp-shadow shader with POINT_SHADOW_RT failed to compile with MaterialBuffer on.
        // UNOFFICIAL 2026-11-12 (ptbump; core/CapturedNormals.h NoHighlightFlag, ShaderRecord's CAPTURED_PT: the DXVK fork with the AlbedoBuffer
        // level, where path tracing runs). The roughness written here is 1 - the normal map's alpha for EVERY object, also for a mesh the
        // game draws with no highlight at all (no specular shader flag: these shaders' plain Lambert variants never use it). Their alpha is
        // often 1 (a DXT1 map has none): roughness 0.04, and the path tracer's first hit, which reads this record, mirrored the sky off
        // rocks and walls the game draws matte. While the path tracer runs, Render.cpp MaterialForPass sends such a draw's c211.w NEGATIVE
        // (the same class + 8 x radius): the *NoHighlight macros then write b = -2 - roughness (-3 .. -2; a missing roughness stays -1),
        // which the path tracer reads as matte and every other reader of b decodes back. Without the flag (path tracing off, native
        // Direct3D 9): w is positive and the records are bit for bit what they were (abs is a source modifier: no instruction).
        #if defined(CAPTURED_PT) && CAPTURED_PT
            #define NVR_MATERIAL_W abs(NVR_SpecFade.w)
            #define CapturedNoHighlight(roughness) (NVR_SpecFade.w < 0.0f ? -2.0f - (roughness) : (roughness))
        #else
            #define NVR_MATERIAL_W NVR_SpecFade.w
        #endif
        #define NVR_MATERIAL_RADIUS floor(NVR_MATERIAL_W * 0.125f)
        #define NVR_MATERIAL_CLASS (NVR_MATERIAL_W - 8.0f * NVR_MATERIAL_RADIUS)
        #if CAPTURED_NORMALS_ON >= 4
        // UNOFFICIAL 2026-11-14 GBufferV2 ([Main.Main.Quality] GBufferV2, restart; CAPTURED_NORMALS_ON 4; docs/next/FULL-DEFERRED-RENDERER-PLAN.md
        // 4.2, docs/next/FD-GBUFFER-RESULTS.md). COLOR2 becomes:
        //   r  the shading model in the integer part (the classes as before: 1 object, 2 gear, 3 cloth, 4 land, 5 plants, 6 skin, 7 grass; new:
        //      8 hair, 9 eye, 10 unlit) + 16 when the record lights lamps with a highlight (the SPECULAR variants; the others light them with
        //      diffuse only), + 0.49 x metal in the fraction. model = floor(r + 0.01) - 16 x (that >= 16); metal = frac(r) / 0.49.
        //   g  the shading normal (normal-mapped, world axes) octahedral x; 4 (GBV2_NO_NORMAL) = none here (readers take COLOR3's smooth normal)
        //   b  the roughness, as before (-1 none; the path tracer's -3 .. -2 code)
        //   a  the shading normal's octahedral y (full fp16); "written" is r > 0.5 (the class), no longer a.
        // The bound radius (g before) is not in it any more.
            #define GBV2_SPEC_BIT 16.0f
            #define GBV2_NO_NORMAL 4.0f
            #if defined(HAIR)
                #define GBV2_CLASS 8.0f
            #elif defined(NVR_EYE)
                #define GBV2_CLASS 9.0f
            #else
                #define GBV2_CLASS NVR_MATERIAL_CLASS
            #endif
            #if defined(SPECULAR)
                #define GBV2_SPEC GBV2_SPEC_BIT
            #else
                #define GBV2_SPEC 0.0f
            #endif
            #define CapturedMaterialOut(roughness) float4(GBV2_CLASS + GBV2_SPEC, GBV2_NO_NORMAL, (roughness), 1.0f)
            #define CapturedMaterialOutClass(materialClass, roughness) float4((materialClass) + GBV2_SPEC, GBV2_NO_NORMAL, (roughness), 1.0f)
            #define CapturedMaterialOutMetal(roughness, metal) float4(GBV2_CLASS + GBV2_SPEC + 0.49f * saturate(metal), GBV2_NO_NORMAL, (roughness), 1.0f)
            // The shading normal into a record made above: n in world axes, any length (0: none). A draw the hook marked as blended
            // (CapturedCoverage's mark in NVR_SpecFade.z: 3 or -1) keeps a = 1 and records no shading normal: under the SRCALPHA blend the
            // record's alpha is the blend weight of its rgb, so it must stay 1 (or 0, CapturedCoverage) there.
            float4 CapturedShadingNormalZ(float4 material, float3 n, float markZ) {
                const float len2 = dot(n, n);
                const float2 o = ShadowEncodeNormal(n * rsqrt(max(len2, 1.0e-20f)));
                const bool none = len2 < 1.0e-12f || markZ > 1.5f || markZ < -0.5f;
                material.g = none ? GBV2_NO_NORMAL : o.x;
                material.a = none ? 1.0f : o.y;
                return material;
            }
            #define CapturedShadingNormal(material, n) CapturedShadingNormalZ((material), (n), NVR_SpecFade.z)
            // The normal-mapped normal where the vertex shader sends no tangent frame: the cotangent frame of the screen derivatives of the
            // camera-relative world position p and the normal map's uv (Schuler, "Followup: Normal Mapping Without Precomputed Tangents",
            // 2013), about n0 (the smooth or the face normal, any length). tn: the tangent-space normal the shader lit with (x along +u,
            // y along +v: Direct3D's v runs down the texture, the game's maps are Direct3D style). ddx / ddy: call it at the top level only.
            // Mirrored uv flips the frame with it (the sign rides in the cross products). n0 must face the camera: Direct3D's ddy runs DOWN
            // the screen (GLSL's dFdy up), which makes det(n0, dp1, dp2) negative for a facing n0, so the frame is negated (s < 0). Worked
            // check: a wall facing +z seen from +z, u = x, v = -y: t = (-1, 0, 0) and b = (0, 1, 0) before the sign, +x and -y after.
            // (2026-11-16 fd-roofedge) n0 is re-faced by the triangle's side (CapturedTriangleFlip): the callers pass the smooth normal
            // faced by the view test, which points it away from the light under a smooth-shaded ridge seen from below.
            // The frame about a unit n0 on the camera's side of the triangle (dp1 / dp2: ddx / ddy of p); nz: the normal the map's z goes along.
            float3 CapturedDerivFrame(float3 dp1, float3 dp2, float2 uv, float3 n0, float3 nz, float3 tn) {
                const float2 duv1 = ddx(uv), duv2 = ddy(uv);
                const float3 dp2perp = cross(dp2, n0), dp1perp = cross(n0, dp1);
                const float3 t = dp2perp * duv1.x + dp1perp * duv2.x;
                const float3 b = dp2perp * duv1.y + dp1perp * duv2.y;
                const float s = -rsqrt(max(max(dot(t, t), dot(b, b)), 1.0e-30f));
                return (t * tn.x + b * tn.y) * s + nz * tn.z;
            }
            float3 CapturedDerivNormal(float3 p, float2 uv, float3 n0, float3 tn) {
                const float3 dp1 = ddx(p), dp2 = ddy(p);
                n0 *= rsqrt(max(dot(n0, n0), 1.0e-20f)) * CapturedTriangleFlip(n0, p, cross(dp1, dp2));
                return CapturedDerivFrame(dp1, dp2, uv, n0, n0, tn);
            }
            // UNOFFICIAL 2026-11-16 fd-directlight (docs/next/FD-DIRECTLIGHT.md item 1): CapturedDerivNormal for a caller that has the vertex
            // normal as the vertex shader sent it (nVertex: not faced to the camera). Where the triangle is seen from BEHIND (a two-sided card or
            // cloth: the vertex normal lies behind it as the camera sees it), the forward shaders light it with its FRONT's normal-mapped normal
            // (they light in the vertex's tangent frame, which nothing turns round; the game's shaders do the same, and so does the table
            // family's exact frame above), so the record stores that one: the same tangent directions (the uv runs the same way on both sides)
            // with the map's z along the vertex normal instead of the camera-side one. Near Jacobstown pines: needle cards seen from their back
            // were lit as their front forward and lit as a surface facing the camera (away from the sun) by the deferred passes. Everywhere
            // else (a front, a smooth-shaded ridge seen from below, an edge-on triangle) exactly CapturedDerivNormal's normal.
            float3 CapturedDerivNormalFront(float3 p, float2 uv, float3 nVertex, float3 tn, bool allowBack) {
                const float3 dp1 = ddx(p), dp2 = ddy(p);
                const float3 face = cross(dp1, dp2);
                const float side = dot(face, p);	// (CapturedTriangleFlip's test written out: its "usable" is needed here too)
                const bool usable = side * side > 1.0e-8f * dot(face, face) * dot(p, p);
                const float behind = dot(nVertex, face) * side;	// > 0: the vertex normal lies behind the triangle as the camera sees it
                const float3 n0 = nVertex * (rsqrt(max(dot(nVertex, nVertex), 1.0e-20f)) * ((usable ? behind : dot(nVertex, p)) > 0.0f ? -1.0f : 1.0f));
                return CapturedDerivFrame(dp1, dp2, uv, n0, (allowBack && usable && behind > 0.0f) ? -n0 : n0, tn);
            }
        #else
        #define CapturedMaterialOut(roughness) float4(NVR_MATERIAL_CLASS, NVR_MATERIAL_RADIUS, (roughness), 1.0f)
        #define CapturedMaterialOutClass(materialClass, roughness) float4((materialClass), NVR_MATERIAL_RADIUS, (roughness), 1.0f)
        #define CapturedMaterialOutMetal(roughness, metal) float4(NVR_MATERIAL_CLASS + 0.49f * saturate(metal), NVR_MATERIAL_RADIUS, (roughness), 1.0f)
        #endif
        // the one-pass and base-pass records of a mesh that may have no highlight (metal pixels keep theirs: the game's metal reflects).
        // Without CAPTURED_PT they ARE the plain forms, token for token (the compiled shaders are the same bytes as before: shcheck).
        #if defined(CAPTURED_PT) && CAPTURED_PT
            #define CapturedMaterialOutNoHighlight(roughness) CapturedMaterialOut(CapturedNoHighlight(roughness))
            #define CapturedMaterialOutMetalNoHighlight(roughness, metal) CapturedMaterialOutMetal(((metal) > 0.0f ? (roughness) : CapturedNoHighlight(roughness)), (metal))
        #else
            #define CapturedMaterialOutNoHighlight(roughness) CapturedMaterialOut(roughness)
            #define CapturedMaterialOutMetalNoHighlight(roughness, metal) CapturedMaterialOutMetal(roughness, metal)
        #endif
        // UNOFFICIAL 2026-11-12 (ptbump) the LAND's record with its normal-mapped normal for the path tracer's first hit (CAPTURED_PT 2:
        // [Main.Main.Quality] CapturedBumpNormals): n = the blended layers' bumped normal in world axes (the land is not rotated), any
        // length. g = 3 + e.x, b = -7 + e.y, e hemi-octahedral about +z (-1 .. 1 each; a normal below the horizon is laid on it). A land
        // texel's g held the last object's radius and its b -1 (no roughness): no reader takes either from the land (they test the class,
        // or b >= 0), and b stays "no roughness" to them.
        float4 CapturedLandBumpOut(float3 n) {
            n.z = max(n.z, 0.0f);
            const float2 h = n.xy / max(abs(n.x) + abs(n.y) + n.z, 1.0e-6f);
            return float4(4.0f, 3.0f + h.x + h.y, -7.0f + h.x - h.y, 1.0f);
        }
    #else
        #define CapturedMaterialOut(roughness) 0.0f
        #define CapturedMaterialOutClass(materialClass, roughness) 0.0f
        #define CapturedMaterialOutMetal(roughness, metal) 0.0f
        #define CapturedMaterialOutNoHighlight(roughness) 0.0f
        #define CapturedMaterialOutMetalNoHighlight(roughness, metal) 0.0f
    #endif

    // UNOFFICIAL 2026-10-31 AlbedoBuffer (core/CapturedNormals.h; [Main.Main.Quality] AlbedoBuffer, restart, with MaterialBuffer; CAPTURED_NORMALS_ON 3;
    // docs/next/DEFERRED-RENDERING-PLAN.md rung A): the templates' spare COLOR1 = TESR_AlbedoBuffer while the main list draws: rgb the LINEAR base
    // colour the light is multiplied by (texture x vertex colour, after the glow mix, before metal: readers take diffuse = rgb x (1 - metal) and
    // F0 = lerp(0.04, rgb, metal), the metal from TESR_MaterialBuffer.r's fraction), a 1 = written. Passes with no texture of their own (the
    // DIFFUSE lamp passes, unless FoldHighlights bound the base map on s7) write a 0; the ONLY_SPECULAR highlight passes keep their highlightCopy
    // there (no writer: core/CapturedNormals.h masks them off). Below 3 the macros give the 0 the pad was written with before.
    #if CAPTURED_NORMALS_ON >= 3
        #define CapturedAlbedoOut(rgb) float4((rgb), 1.0f)
        #define CAPTURED_ALBEDO_NONE float4(0.0f, 0.0f, 0.0f, 0.0f)
    #else
        #define CapturedAlbedoOut(rgb) 0.0f
        #define CAPTURED_ALBEDO_NONE 0.0f
    #endif

    // UNOFFICIAL 2026-11-12 CapturedCoverage (core/CapturedNormals.h CoverageBegin; [Main.Main.Quality] CapturedCoverage, live;
    // docs/next/PT-COMPAT-AUDIT-2026-11-12.md item 7). The templates wrote their records with alpha 1 whatever the draw's own alpha: an
    // object fading in, a Fallout 3 rubble skirt or road edge fading out by vertex alpha, terrain blending's Smooth seam were recorded as
    // solid up to the polygon's edge, and the path tracer drew a hard-edged solid shape where the game shows a soft fade. For a draw the
    // game BLENDS (SRCALPHA / INVSRCALPHA; the draw hook marks it in NVR_SpecFade.z for that one draw: 3 or -1 in place of 1 or 0, so every
    // z > 0.5 test reads as before) the three records' alpha is 0 where the draw's own alpha is below 0.5: under that blend a record with
    // alpha 0 changes nothing, the texel keeps what was drawn there before (the ground under the skirt), colour, material, normal and
    // distance alike. At 0.5 and above: as before. An opaque draw (z 0 or 1): x 1, bit for bit what it wrote before, whatever its alpha.
    // Needs the AlbedoBuffer level (3: the level the path tracer reads); a macro, so NVR_SpecFade (Includes/PBR.hlsl) is met at the use.
    #if CAPTURED_NORMALS_ON >= 3
        #define CapturedCoverage(alpha) (((NVR_SpecFade.z > 1.5f || NVR_SpecFade.z < -0.5f) && (alpha) < 0.5f) ? 0.0f : 1.0f)
    #endif

    // UNOFFICIAL 2026-11-12 foliage records (docs/next/PT-COMPAT-AUDIT-2026-11-12.md items 1 and 9; core/CapturedNormals.h GrassWriters /
    // LeafWriters): the grass (GRASS23x000 / 000TMS.pso) and SpeedTree leaf (STLEAF2000 / 2001 (TMS).pso) pixel shaders are no templates and
    // wrote no record: their pixels kept the record of whatever was drawn behind them (or none), with their own depth. With the AlbedoBuffer
    // level they write all three: the linear texture colour, class 5 (plants; no metal) with a matte roughness and a small thickness (they
    // have no NVR_SpecFade: no per-draw radius), and a normal with the camera distance. `written` 0 (the game's vertex shader: no position; a
    // texel below half coverage) gives alpha 0 on all three: unwritten on an opaque draw, and under the grass's SRCALPHA / INVSRCALPHA blend
    // the texel is left as it was. The targets are bound for them only while the path tracer replaces the picture.
    #if CAPTURED_NORMALS_ON >= 3
        #define CAPTURED_FOLIAGE 1
        #define CAPTURED_FOLIAGE_OUTPUTS float4 capturedPad1 : COLOR1; float4 capturedPad2 : COLOR2; float4 capturedNormal : COLOR3;
        #define CapturedFoliageAlbedo(rgb, written) float4((rgb), (written))
        #define CapturedFoliageMaterial(roughness, written) float4(5.0f * (written), 8.0f, (roughness), (written))
        float4 CapturedFoliageNormal(float3 n, float3 worldPos, float written) {
            float4 o = CapturedNormalOut(n, worldPos);
            o.a = min(o.a, 1.0f) * written;	// (never the actors' 2)
            return o;
        }
    #endif
#endif

#endif
