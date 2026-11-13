#include "render/gpu/parallax.h"
#include "render/gpu/material_settings.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::fabs(a - b) <= tolerance;
    }

    bool nearVec(const Vec2& a, const Vec2& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance);
    }

    const ParallaxHeight flat = [](const Vec2&) { return 1.0f; };

    /*
    * A vector in the surface's own frame: x along the tangent, y along the bitangent (the tangent's sign times the normal
    * crossed with it), z along the normal; the frame made orthonormal first, since interpolated normals and tangents are
    * neither unit length nor quite square to each other.
    */
    void testTangentFrame() {
        const Vec3 normal{0.0f, 0.0f, 2.0f};
        const Vec4 tangent{1.0f, 0.0f, 0.3f, 1.0f};
        assert(near(toTangentFrame(Vec3{1.0f, 0.0f, 0.0f}, normal, tangent).x, 1.0f, 1e-6f));
        assert(near(toTangentFrame(Vec3{0.0f, 1.0f, 0.0f}, normal, tangent).y, 1.0f, 1e-6f));
        assert(near(toTangentFrame(Vec3{0.0f, 0.0f, 1.0f}, normal, tangent).z, 1.0f, 1e-6f));

        // A mirrored frame (w -1) turns the bitangent round.
        assert(near(toTangentFrame(Vec3{0.0f, 1.0f, 0.0f}, normal, Vec4{1.0f, 0.0f, 0.0f, -1.0f}).y, -1.0f, 1e-6f));

        const Vec3 turned = toTangentFrame(Vec3{0.3f, -0.5f, 0.8f}.normalized(), normal, tangent);
        assert(near(turned.length(), 1.0f, 1e-5f));
    }

    /*
    * Steps: the fewest looking straight down, more towards grazing views, where the ray crosses more of the height map,
    * never past the setting's most; the whole shift at the deepest point is the view's sideways part over its part along
    * the normal, times the depth, away from the eye, its cosine kept above the smallest so a grazing view stays finite.
    */
    void testStepsAndShift() {
        assert(parallaxSteps(1.0f, 32) == parallaxFewestSteps);
        assert(parallaxSteps(0.0f, 32) == 32);
        assert(parallaxSteps(0.5f, 32) > parallaxFewestSteps && parallaxSteps(0.5f, 32) < 32);
        assert(parallaxSteps(0.2f, 4) == parallaxFewestSteps);

        const Vec3 view = Vec3{0.6f, 0.0f, 0.8f};
        assert(nearVec(parallaxDelta(view, 0.04f), Vec2{-0.6f / 0.8f * 0.04f, 0.0f}, 1e-7f));
        assert(nearVec(parallaxDelta(Vec3{0.0f, 0.0f, 1.0f}, 0.04f), Vec2{}, 0.0f));

        const Vec2 grazing = parallaxDelta(Vec3{1.0f, 0.0f, 0.0f}, 0.04f);
        assert(std::isfinite(grazing.x) && near(grazing.x, -0.04f / parallaxSmallestViewCosine, 1e-6f));

        // Light rises towards its source instead.
        assert(nearVec(parallaxLightDelta(view, 0.04f), Vec2{0.6f / 0.8f * 0.04f, 0.0f}, 1e-7f));
    }

    /*
    * The crossing between the last step above the height map and the first below, the surface between them taken as a
    * straight line: a gap of 1 above and 3 below crosses a quarter of the way.
    */
    void testCrossing() {
        assert(near(parallaxGap(0.25f, 0.5f), 0.25f, 1e-7f));
        assert(near(parallaxCrossing(1.0f, -3.0f), 0.25f, 1e-7f));
        assert(near(parallaxCrossing(0.5f, 0.0f), 1.0f, 1e-7f));
        assert(near(parallaxCrossing(0.0f, -1.0f), 0.0f, 1e-7f));
    }

    // A flat height map at the top shifts nothing, from any view.
    void testFlatShiftsNothing() {
        for (const Vec3 view : {Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.6f, 0.0f, 0.8f}, Vec3{-0.3f, 0.9f, 0.3f}.normalized()}) {
            const ParallaxHit hit = parallaxSearch(flat, Vec2{0.3f, 0.7f}, view, 0.05f, 16);
            assert(nearVec(hit.uv, Vec2{0.3f, 0.7f}, 0.0f) && hit.height == 1.0f);
        }
    }

    /*
    * A tilted plane, height 1 - u / 2 (falling away along u), seen at an angle: the refined hit is the exact crossing of
    * the ray and the plane, within a small share of a step; a plain search without the refinement would be off by up
    * to a step.
    */
    void testTiltedPlane() {
        const ParallaxHeight tilted = [](const Vec2& uv) { return std::clamp(1.0f - uv.x * 0.5f, 0.0f, 1.0f); };
        const Vec3 view = Vec3{-0.6f, 0.0f, 0.8f}; // the eye towards -u, so the ray runs towards +u
        const float depth = 0.2f;
        const Vec2 start{0.4f, 0.5f};
        const ParallaxHit hit = parallaxSearch(tilted, start, view, depth, 16);

        // Exactly: along the ray uv.x = 0.4 + 0.15 t (0.6 / 0.8 * 0.2) and the ray's height 1 - t meets 1 - uv.x / 2.
        const float exact = 0.2f / (1.0f - 0.075f);
        const float exactU = start.x + 0.15f * exact;
        assert(near(hit.uv.x, exactU, 1e-4f) && near(hit.uv.y, 0.5f, 1e-6f));
        assert(near(hit.height, 1.0f - exact, 1e-4f));
    }

    /*
    * A brick's edge: the brick top (height 1) for u under 0.5 and mortar (height 0) past it, seen at a grazing angle from
    * the brick's side. A ray starting over the mortar just past the edge goes down into the mortar; one starting over the
    * mortar seen from the far side, towards the brick, hits the brick's wall: the mortar behind it hidden.
    */
    void testBrickEdge() {
        const ParallaxHeight brick = [](const Vec2& uv) { return uv.x < 0.5f ? 1.0f : 0.0f; };
        const float depth = 0.05f;

        // Looking from +u (the eye on the mortar's side) towards the brick: the ray runs towards -u, into the brick's
        // side, so a point a little past the edge shows the brick's wall, not the mortar's floor.
        const Vec3 fromMortarSide = Vec3{0.8f, 0.0f, 0.6f};
        const ParallaxHit wall = parallaxSearch(brick, Vec2{0.52f, 0.5f}, fromMortarSide, depth, 32);
        assert(wall.height > 0.0f && wall.uv.x < 0.5f + 1e-3f);

        // From the brick's side the ray runs towards +u, away from it: it reaches the mortar's floor.
        const Vec3 fromBrickSide = Vec3{-0.8f, 0.0f, 0.6f};
        const ParallaxHit floor = parallaxSearch(brick, Vec2{0.52f, 0.5f}, fromBrickSide, depth, 32);
        assert(near(floor.height, 0.0f, 1e-5f) && floor.uv.x > 0.52f);

        // On top of the brick nothing moves.
        const ParallaxHit top = parallaxSearch(brick, Vec2{0.2f, 0.5f}, fromMortarSide, depth, 32);
        assert(top.height == 1.0f && top.uv.x == 0.2f);
    }

    /*
    * Self-shadowing: from the mortar's floor next to a brick, a low light beyond the brick is blocked by it, a high one
    * (straight down) is not, and a low light from the open side reaches it. On top of a brick nothing blocks. Each
    * sample's share falls from 1 as the terrain pokes above the ray, by how far it pokes per height the ray has climbed.
    */
    void testSelfShadow() {
        assert(parallaxShadowSample(0.3f, 0.5f, 0.2f) == 1.0f);
        assert(near(parallaxShadowSample(0.55f, 0.5f, 0.5f), 1.0f - 0.1f * parallaxShadowSoftness, 1e-6f));
        assert(parallaxShadowSample(1.0f, 0.2f, 0.1f) == 0.0f);

        const ParallaxHeight brick = [](const Vec2& uv) { return uv.x < 0.5f ? 1.0f : 0.0f; };
        const float depth = 0.05f;
        const Vec2 mortar{0.53f, 0.5f};

        const Vec3 lowBeyondBrick = Vec3{-0.9f, 0.0f, 0.3f}.normalized();
        assert(parallaxShadow(brick, mortar, 0.0f, lowBeyondBrick, depth, parallaxShadowSteps) < 0.01f);

        assert(parallaxShadow(brick, mortar, 0.0f, Vec3{0.0f, 0.0f, 1.0f}, depth, parallaxShadowSteps) == 1.0f);

        const Vec3 lowOpenSide = Vec3{0.9f, 0.0f, 0.3f}.normalized();
        assert(parallaxShadow(brick, mortar, 0.0f, lowOpenSide, depth, parallaxShadowSteps) == 1.0f);

        assert(parallaxShadow(brick, Vec2{0.2f, 0.5f}, 1.0f, lowBeyondBrick, depth, parallaxShadowSteps) == 1.0f);
    }

    /*
    * The fade with distance: the depth seen head-on in pixels (the depth over the larger of the texture's steps to the
    * next pixel), full past PARALLAX_FADE_FULL_PIXELS, none under PARALLAX_FADE_NONE_PIXELS, smooth between.
    */
    void testFade() {
        assert(near(parallaxDepthPixels(0.04f, Vec2{0.001f, 0.0f}, Vec2{0.0f, 0.002f}), 20.0f, 1e-4f));
        assert(parallaxDepthPixels(0.04f, Vec2{}, Vec2{}) > 1e6f);
        assert(parallaxFade(parallaxFadeFullPixels) == 1.0f && parallaxFade(100.0f) == 1.0f);
        assert(parallaxFade(parallaxFadeNonePixels) == 0.0f && parallaxFade(0.0f) == 0.0f);
        const float middle = parallaxFade(0.5f * (parallaxFadeFullPixels + parallaxFadeNonePixels));
        assert(middle > 0.2f && middle < 0.8f);
    }

    // The settings: on, self-shadowed, 32 steps at most, between the fewest and the hard limit.
    void testSettings() {
        const MaterialSettings defaults;
        assert(defaults.clearCoat && defaults.parallax && defaults.parallaxSelfShadows && defaults.parallaxSteps == 32);
        assert(parallaxFewestSteps < 32 && parallaxMostSteps >= 32);
    }
}

void testParallax() {
    testTangentFrame();
    testStepsAndShift();
    testCrossing();
    testFlatShiftsNothing();
    testTiltedPlane();
    testBrickEdge();
    testSelfShadow();
    testFade();
    testSettings();
}
