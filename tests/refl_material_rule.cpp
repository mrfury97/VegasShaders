// Checks the MaterialAware reflections' per-pass alpha rule (src/core/ReflMaterialRule.h, perf-mirrorfix 2026-11-07) on a simulated
// pixel of the world target's alpha: a mirror panel drawn first (its material), then what is drawn in front of it. Before the fix every
// pass that was not a material pass left the alpha alone (Keep), so a shirt, a fence post or hair drawn over the mirror kept the mirror's
// material (the tester's chrome shirt at the VSRTRoom mirror wall); the controls below show that leak with the old rule.
// Build: cl /nologo /EHsc /std:c++17 tests\refl_material_rule.cpp /Fe<out>\refl_material_rule.exe   (exit code = failures)
#include <cstdio>
#include <cmath>
#include "../src/core/ReflMaterialRule.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); failures++; } else { std::printf("PASS: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using namespace ReflMaterialRule;

// The rule before 2026-11-07: material passes only, everything else Keep
static Write OldDecide(const Pass& p) { return Decide(p) == Material ? Material : Keep; }

static Pass World(bool hasMap) { Pass p; p.live = true; p.world = true; p.hasGeometry = true; p.hasMap = hasMap; return p; }

int main() {
	const float mirror = Encode(3, 0.02f);	// vs_mirror01: metal, RoughnessOverrides 0.02
	CHECK(IsMaterial(mirror), "the mirror's alpha %.4f is a material", mirror);
	CHECK(!IsMaterial(0.0f) && !IsMaterial(1.0f) && !IsMaterial(3.0f), "0, 1 and 3 are no material");

	// 1. the mirror panel itself: a material pass
	Pass panel = World(true);
	CHECK(Decide(panel) == Material, "the mirror panel's first pass (an _rmaos map, opaque, untested) writes its material");
	float a = After(Decide(panel), 0.0f, 1.0f, mirror, false);
	CHECK(IsMaterial(a) && std::fabs(a - mirror) < 1e-6f, "after the panel: %.4f", a);

	// 2. a static object without a map in front of it (the fence post): Own -> no material
	Pass post = World(false);
	CHECK(Decide(post) == Own, "a static draw without a map writes its own alpha");
	float b = After(Decide(post), a, 1.0f, mirror, false);
	CHECK(!IsMaterial(b), "fence post over the mirror: alpha %.4f, no material", b);
	CHECK(IsMaterial(After(OldDecide(post), a, 1.0f, mirror, false)), "control: the old rule leaked the mirror onto the post");

	// 3. a person's shirt (skinned, opaque, no map)
	Pass shirt = World(false); shirt.skinned = true;
	CHECK(Decide(shirt) == Own, "a skinned opaque draw writes its own alpha");
	CHECK(!IsMaterial(After(Decide(shirt), a, 1.0f, mirror, false)), "shirt over the mirror: no material");
	CHECK(IsMaterial(After(OldDecide(shirt), a, 1.0f, mirror, false)), "control: the old rule made the shirt chrome");

	// 4. an alpha-tested card (a fence's chain-link, a plant): its drawn texels write their alpha (>= the reference, <= 1)
	Pass card = World(true); card.testProperty = true;
	CHECK(Decide(card) == Own, "an alpha-tested draw (even with a map) writes its own alpha, never a material");
	CHECK(!IsMaterial(After(Decide(card), a, 0.6f, mirror, false)), "a cut-out texel over the mirror: no material");

	// 5. the lamp / highlight passes drawn over an object: blended by the device, the scene's separate alpha keeps the alpha
	Pass lamp = World(true); lamp.firstKind = false;
	CHECK(Decide(lamp) == Own, "a lamp pass gets alpha writes (its blend keeps the alpha)");
	CHECK(std::fabs(After(Decide(lamp), a, 1.0f, mirror, true) - a) < 1e-6f, "a lamp pass over the mirror keeps its material");
	Pass noSeparate = lamp; noSeparate.separateAlpha = false;
	CHECK(Decide(noSeparate) == Keep, "separate alpha blending off: alpha left alone (an additive pass would add up to a material)");

	// 6. hair: a blended first pass of a skinned geometry (alpha test + blend): covers the material by its alpha
	Pass hair = World(false); hair.skinned = true; hair.blendProperty = true; hair.testProperty = true;
	CHECK(Decide(hair) == Cover, "hair (blended, skinned) covers");
	CHECK(!IsMaterial(After(Decide(hair), a, 1.0f, mirror, true)), "a solid hair texel over the mirror: no material");
	CHECK(!IsMaterial(After(Decide(hair), a, 0.5f, mirror, true)), "a half hair texel over the mirror: no material (%.3f)", After(Decide(hair), a, 0.5f, mirror, true));
	CHECK(IsMaterial(After(Decide(hair), a, 0.0f, mirror, true)), "a see-through hair texel keeps the mirror (it shows through)");
	CHECK(IsMaterial(After(OldDecide(hair), a, 1.0f, mirror, true)), "control: the old rule painted the mirror on the hair");
	Pass hairLamp = hair; hairLamp.firstKind = false;
	CHECK(Decide(hairLamp) == Keep, "hair's lamp passes leave the alpha alone");

	// 7. glass / decals (blended, static): Keep (a mirror seen through a window still reflects)
	Pass glass = World(false); glass.blendProperty = true;
	CHECK(Decide(glass) == Keep, "a blended static draw (glass, decal) keeps the material under it");

	// 8. depth-only passes write no colour: Keep
	Pass depth = World(false); depth.colorWrite = 0;
	CHECK(Decide(depth) == Keep, "a depth-only pass gets no alpha writes");

	// 9. the first-person scene: no material passes; the arms and gun write their own alpha
	Pass gun = World(true); gun.world = false;
	CHECK(Decide(gun) == Own, "first person, even with a map: own alpha (no material)");
	CHECK(!IsMaterial(After(Decide(gun), a, 1.0f, mirror, false)), "the gun over the mirror: no material");

	// 10. MaterialAware off / no geometry: nothing changes
	Pass off = World(true); off.live = false;
	CHECK(Decide(off) == Keep, "MaterialAware off: Keep");
	Pass none = World(true); none.hasGeometry = false;
	CHECK(Decide(none) == Keep, "no geometry: Keep");

	// 11. a refused pass kind (drew blended with the flag once): never a material, own alpha (its blend keeps it)
	Pass refused = World(true); refused.refused = true;
	CHECK(Decide(refused) == Own, "a refused pass: own alpha");

	// 12. MSAA edge: half shirt, half mirror averaged: below 2, no material
	CHECK(!IsMaterial(0.5f * mirror + 0.5f * 1.0f), "an MSAA edge between the shirt and the mirror resolves to no material");

	// --- perf-mirrorfix2 (test 10): the rule at draw time, from the device's state (DecideDraw) -----------------------------------------
	auto draw = [](bool blend, uint32_t src, bool skinned) { Draw d; d.live = true; d.colorWrite = 7; d.blend = blend; d.srcBlend = src; d.skinned = skinned; return d; };
	const uint32_t kOne = 2, kDestColor = 9, kSrcAlpha = kBlendSrcAlpha;
	// her head in front of the mirror, drawn by the sorted blended list (never through the pass hooks): before, nothing touched the alpha
	float px = mirror;
	px = After(DecideDraw(draw(false, kOne, true)), px, 1.0f, mirror, false);	// the face's first (opaque) skin pass
	CHECK(DecideDraw(draw(false, kOne, true)) == Own && !IsMaterial(px), "draw: the face's opaque skin pass writes its own alpha (%.3f)", px);
	px = After(DecideDraw(draw(true, kOne, true)), px, 1.0f, mirror, true);		// its additive light passes
	px = After(DecideDraw(draw(true, kDestColor, true)), px, 1.0f, mirror, true);	// the FaceGen colour multiply (DESTCOLOR)
	CHECK(DecideDraw(draw(true, kOne, true)) == Keep && DecideDraw(draw(true, kDestColor, true)) == Keep && !IsMaterial(px),
		"draw: the face's additive light and FaceGen multiply passes keep its own alpha: no material on the face");
	// an actor drawn blended over (its fade property: SRCALPHA / INVSRCALPHA) at full opacity
	CHECK(DecideDraw(draw(true, kSrcAlpha, true)) == Cover && !IsMaterial(After(Cover, mirror, 1.0f, mirror, true)), "draw: a skinned SRCALPHA draw covers the mirror");
	// hair in the blended list
	CHECK(!IsMaterial(After(DecideDraw(draw(true, kSrcAlpha, true)), mirror, 0.9f, mirror, true)), "draw: hair (skinned, SRCALPHA) over the mirror: no material");
	CHECK(IsMaterial(After(Keep, mirror, 0.9f, mirror, true)), "control: a draw nobody handled (the blended list before) kept the mirror");
	// glass, decals: static SRCALPHA keeps; a static opaque draw writes
	CHECK(DecideDraw(draw(true, kSrcAlpha, false)) == Keep, "draw: static glass / decal (SRCALPHA) keeps the material under it");
	CHECK(DecideDraw(draw(false, kOne, false)) == Own, "draw: a static opaque draw writes its own alpha");
	// alpha already on (a material pass, or a pass hook's Own): left alone
	Draw mat = draw(false, kOne, false); mat.colorWrite = 15;
	CHECK(DecideDraw(mat) == Material, "draw: alpha writes already on: left as they are");
	Draw depthOnly = draw(false, kOne, true); depthOnly.colorWrite = 0;
	CHECK(DecideDraw(depthOnly) == Keep, "draw: no colour writes: Keep");
	Draw noSep = draw(true, kSrcAlpha, true); noSep.separateAlpha = false;
	CHECK(DecideDraw(noSep) == Keep, "draw: separate alpha blending off: Keep");
	Draw dead = draw(false, kOne, true); dead.live = false;
	CHECK(DecideDraw(dead) == Keep, "draw: MaterialAware off / not the world or first-person scene: Keep");

	std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return failures;
}
