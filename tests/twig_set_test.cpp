#include "scene/twig_set.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "twig_helpers.h"

namespace {
    /*
    * A twig set keeps every twig once, in bricks of at most 64 that run through them in order, each brick's sphere
    * holding each of its twigs' spheres; near twigs share bricks, so a brick is far smaller than the crown; the same
    * twigs make the same set; a prototype without a reach is refused.
    */
    void testBricksHoldTheirTwigs() {
        const std::vector<TwigInstance> given = scatteredTwigs(5000);
        const std::vector<float> reaches{1.2f, 1.0f, 0.8f};
        const TwigSet set = makeTwigSet({MeshHandle{3}, MeshHandle{4}, MeshHandle{5}}, reaches, given);
        assert(set.prototypes.size() == 3 && set.twigs.size() == given.size());

        std::vector<std::array<float, 4>> before;
        std::vector<std::array<float, 4>> after;

        for (std::size_t index = 0; index < given.size(); ++index) {
            before.push_back({given[index].foot.x, given[index].foot.y, given[index].foot.z, given[index].scale});
            after.push_back({set.twigs[index].foot.x, set.twigs[index].foot.y, set.twigs[index].foot.z, set.twigs[index].scale});
        }

        std::sort(before.begin(), before.end());
        std::sort(after.begin(), after.end());
        assert(before == after);

        std::uint32_t next = 0;
        float largest = 0.0f;
        float total = 0.0f;

        for (const TwigBrick& brick : set.bricks) {
            assert(brick.firstTwig == next && brick.twigCount > 0 && brick.twigCount <= twigBrickSize);
            next += brick.twigCount;

            for (std::uint32_t twig = brick.firstTwig; twig < brick.firstTwig + brick.twigCount; ++twig) {
                const TwigInstance& placed = set.twigs[twig];
                const float reach = reaches[placed.prototype] * placed.scale;
                assert((placed.foot - brick.centre).length() + reach <= brick.radius * 1.0001f + 1e-5f);
            }

            largest = std::max(largest, brick.radius);
            total += brick.radius;
        }

        assert(next == set.twigs.size() && set.bricks.size() == (given.size() + twigBrickSize - 1) / twigBrickSize);
        // The crown's half diagonal is about 14.4 m (20 x 5 x 20): bricks average under 0.35 of it and none passes half
        // (a space-filling curve's runs made one of 12.6 m).
        const float crown = std::sqrt(10.0f * 10.0f + 2.5f * 2.5f + 10.0f * 10.0f);
        assert(total / static_cast<float>(set.bricks.size()) < 0.35f * crown && largest < 0.5f * crown);

        const TwigSet again = makeTwigSet({MeshHandle{3}, MeshHandle{4}, MeshHandle{5}}, reaches, given);

        for (std::size_t index = 0; index < set.twigs.size(); ++index) {
            assert((set.twigs[index].foot - again.twigs[index].foot).lengthSquared() == 0.0f);
        }

        bool refused = false;

        try {
            makeTwigSet({MeshHandle{3}}, {1.0f}, given);
        } catch (const std::invalid_argument&) {
            refused = true;
        }

        assert(refused);
        assert(makeTwigSet({}, {}, {}).bricks.empty());
    }

    // A twig puts its prototype's x, y and z along its frame, times its scale, from its foot; a mesh reaches its farthest vertex.
    void testTwigPointsAndReach() {
        const TwigInstance twig{.foot = Vec3{1.0f, 2.0f, 3.0f},
                                .across = Vec3{0.0f, 0.0f, -1.0f},
                                .towardsTip = Vec3{0.0f, 1.0f, 0.0f},
                                .facing = Vec3{1.0f, 0.0f, 0.0f},
                                .scale = 2.0f,
                                .prototype = 0,
                                .lean = Vec3{}};
        assert((twigPoint(twig, Vec3{1.0f, 0.5f, 0.25f}) - Vec3{1.5f, 3.0f, 1.0f}).length() < 1e-6f);

        IndexedMesh mesh;
        mesh.vertices.push_back(MeshVertex{Vec3{0.0f, 1.0f, 0.0f}, Vec3{}, Vec2{}, Vec4{}});
        mesh.vertices.push_back(MeshVertex{Vec3{0.0f, 0.0f, -3.0f}, Vec3{}, Vec2{}, Vec4{}});
        assert(meshReach(mesh) == 3.0f && meshReach(IndexedMesh{}) == 0.0f);
    }
}

void testTwigSet() {
    testBricksHoldTheirTwigs();
    testTwigPointsAndReach();
}
