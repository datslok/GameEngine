#include "render/gpu/depth_of_field.h"
#include "render/gpu/transparency.h"

#include <cassert>
#include <cmath>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::abs(a - b) <= tolerance;
    }

    /*
    * A point behind frosted glass spreads over a disc on the pane whose radius grows with how far behind it lies, from
    * none right behind the glass to a ceiling, the spread seen across the camera's pixels, for the infinitely far; the
    * picture's level is log2 of the disc's width, as before.
    */
    void testRadius() {
        const float spread = 0.125f;
        const float perRadian = 1000.0f;
        assert(frostedRadius(spread, 0.0f, 2.0f, perRadian) == 0.0f);
        assert(near(frostedRadius(spread, 2.0f, 2.0f, perRadian), 62.5f, 1e-4f));
        assert(frostedRadius(spread, 1.0f, 2.0f, perRadian) < frostedRadius(spread, 10.0f, 2.0f, perRadian));
        assert(frostedRadius(spread, 1e9f, 2.0f, perRadian) <= frostedReach(spread, perRadian));
        assert(near(frostedRadius(spread, 1e9f, 2.0f, perRadian), frostedReach(spread, perRadian), 1e-3f));
        assert(frostedReach(spread, perRadian) == 125.0f);
        for (const float behind : {0.3f, 1.0f, 7.0f}) {
            const float level = refractionLevel(spread, behind, 2.0f, perRadian, 12);
            assert(near(level, std::log2(2.0f * frostedRadius(spread, behind, 2.0f, perRadian)), 1e-5f));
        }
    }

    // The gather's level: its rings' spacing about a texel, so the reach over DEPTH_OF_FIELD_RINGS halvings' texels.
    void testGatherLevel() {
        assert(frostedGatherLevel(2.0f, 12) == 0.0f);
        assert(near(frostedGatherLevel(16.0f, 12), 2.0f, 1e-6f));
        assert(near(frostedGatherLevel(64.0f, 12), 4.0f, 1e-6f));
        assert(frostedGatherLevel(1e6f, 5) == 4.0f);
    }

    // The shared scatter weight: a disc's light over its area, per unit of the reach's.
    void testScatterWeight() {
        assert(near(scatterWeight(8.0f, 2.0f, 8.0f), 1.0f, 1e-6f));
        assert(near(scatterWeight(-4.0f, 2.0f, 8.0f), 4.0f, 1e-6f));
        assert(scatterWeight(4.0f, 6.0f, 8.0f) == 0.0f);
        assert(near(scatterWeight(0.2f, 0.0f, 8.0f), 0.7f * 64.0f, 1e-4f));
    }

    // A pane's picture behind: a near thing (radius 0) left of x = 32, a far black background right of it with a bright
    // glint in a stripe from x 38 to 45 (as wide as two of the level's texels the gather reads), read at the nearest pixel
    // to each sample.
    struct Scene {
        float farRadius = 15.0f;
        Vec3 object{0.1f, 0.1f, 0.1f};

        FrostedSample at(int x) const {
            if (x < 32) {
                return FrostedSample{object, 0.0f};
            }
            if (x >= 38 && x < 46) {
                return FrostedSample{Vec3{1000.0f, 1000.0f, 1000.0f}, farRadius};
            }
            return FrostedSample{Vec3{}, farRadius};
        }

        Vec3 gather(int x, float reach, float level) const {
            const float place = static_cast<float>(x) + 0.5f;
            return frostedGatherAt(at(x), reach, level, [&](Vec2 offset) { return at(static_cast<int>(std::floor(place + offset.x))); });
        }
    };

    /*
    * The gather: a background at one distance is its own light; a thing right behind the pane stays sharp; and a far
    * glint's glow, which each pixel's own distance used to stop at the near thing's outline, carries on over it.
    */
    void testGather() {
        {
            const FrostedSample same{Vec3{0.3f, 0.6f, 0.9f}, 10.0f};
            const Vec3 colour = frostedGatherAt(same, 16.0f, 2.0f, [&](Vec2) { return same; });
            assert(near(colour.x, 0.3f, 1e-5f) && near(colour.y, 0.6f, 1e-5f) && near(colour.z, 0.9f, 1e-5f));
        }

        const Scene scene;
        const float reach = 16.0f;
        const float level = frostedGatherLevel(reach, 12);

        // Deep in the near thing, beyond the glint's reach: its own light exactly.
        const Vec3 inside = scene.gather(10, reach, level);
        assert(inside.x == 0.1f && inside.y == 0.1f);

        // Beside the glint on the background: its glow.
        const float background = scene.gather(32, reach, level).x;
        assert(background > 10.0f);

        // Across the outline, on the near thing: still much of it, where gathering by the pixel's own distance gave none.
        const float onObject = scene.gather(31, reach, level).x;
        assert(onObject > 0.4f * background);

        // Fading into the near thing as the glint's discs reach less of it.
        assert(scene.gather(27, reach, level).x < onObject && scene.gather(27, reach, level).x > 0.1f);
    }
}

void testFrostedGlass() {
    testRadius();
    testGatherLevel();
    testScatterWeight();
    testGather();
}
