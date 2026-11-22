#include "scene/sway_bones.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <vector>

namespace {
    // A bent line: 3 m up, then 2 m along x, then 1 m up again (6 m long).
    std::vector<Vec3> bentLine() {
        return {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 3.0f, 0.0f}, Vec3{2.0f, 3.0f, 0.0f}, Vec3{2.0f, 4.0f, 0.0f}};
    }

    /*
    * A chain cut into bones about boneMetres long, all the same length along it (a whole number of them, at least
    * one): each bone's pivot the last one's tip, the first's on the line's start, the last's tip on its end, each its
    * parent's child (the first the given parent), its level and its pivot's radius taken along.
    */
    void testAChainIsCut() {
        std::vector<SwayBone> bones{SwayBone{.pivot = Vec3{}, .tip = Vec3{0.0f, -1.0f, 0.0f}}};
        const std::vector<float> radii{0.4f, 0.3f, 0.2f, 0.1f};
        const SwayChain chain = appendSwayChain(bones, bentLine(), radii, 0, 1, 2.0f);
        assert(chain.firstBone == 1 && chain.boneCount == 3 && near(chain.length, 6.0f, 1e-5f));
        assert(bones.size() == 4 && swayBonesOrdered(bones));

        for (int k = 0; k < 3; ++k) {
            const SwayBone& bone = bones[static_cast<std::size_t>(1 + k)];
            assert(bone.parent == k && bone.level == 1);
        }

        assert((bones[1].pivot - Vec3{0.0f, 0.0f, 0.0f}).length() < 1e-5f && (bones[1].tip - Vec3{0.0f, 2.0f, 0.0f}).length() < 1e-5f);
        assert((bones[2].pivot - bones[1].tip).length() < 1e-5f && (bones[2].tip - Vec3{1.0f, 3.0f, 0.0f}).length() < 1e-5f);
        assert((bones[3].tip - Vec3{2.0f, 4.0f, 0.0f}).length() < 1e-5f);
        assert(near(bones[1].radius, 0.4f, 1e-5f) && near(bones[2].radius, 0.4f - 0.1f * (2.0f / 3.0f), 1e-5f));

        // Shorter than a bone: one bone; 7 m at 2 m a bone rounds to 4 bones (1.75 m each), 5 m to 2 or 3.
        std::vector<SwayBone> more;
        const SwayChain one = appendSwayChain(more, {Vec3{}, Vec3{0.0f, 0.5f, 0.0f}}, {0.1f, 0.1f}, -1, 0, 2.0f);
        assert(one.boneCount == 1 && more.size() == 1 && more[0].parent == -1);
        const SwayChain four = appendSwayChain(more, {Vec3{}, Vec3{0.0f, 7.0f, 0.0f}}, {0.1f, 0.1f}, 0, 1, 2.0f);
        assert(four.firstBone == 1 && four.boneCount == 4 && more[1].parent == 0 && more[2].parent == 1);
    }

    // The arc of a point: along the line to the line's closest point to it.
    void testTheArcOfAPoint() {
        const std::vector<Vec3> line = bentLine();
        assert(near(swayArcOfPoint(line, Vec3{0.3f, 1.0f, 0.2f}), 1.0f, 1e-5f));
        assert(near(swayArcOfPoint(line, Vec3{1.0f, 3.4f, -0.3f}), 4.0f, 1e-5f));
        assert(near(swayArcOfPoint(line, Vec3{0.0f, -2.0f, 0.0f}), 0.0f, 1e-5f));
        assert(near(swayArcOfPoint(line, Vec3{2.0f, 9.0f, 0.0f}), 6.0f, 1e-5f));
    }

    /*
    * A vertex along a chain follows the bone it lies in, blended towards that bone's parent over the first blendShare
    * of the bone from its pivot (weight 0 at the pivot, so a joint is followed by the bone before it alone, as the end
    * of the bone before is): continuous along the whole chain. A root bone has no parent to blend with.
    */
    void testTheBindingAlongAChain() {
        std::vector<SwayBone> bones{SwayBone{.pivot = Vec3{}, .tip = Vec3{0.0f, 1.0f, 0.0f}}};
        const SwayChain chain = appendSwayChain(bones, {Vec3{}, Vec3{0.0f, 6.0f, 0.0f}}, {0.1f, 0.1f}, 0, 1, 2.0f);
        assert(chain.boneCount == 3);

        const SwayBinding atPivot = swayChainBinding(chain, bones, 0.0f, 0.5f);
        assert(atPivot.bone == 1 && atPivot.weight == 0.0f);
        const SwayBinding quarter = swayChainBinding(chain, bones, 0.5f, 0.5f);
        assert(quarter.bone == 1 && near(quarter.weight, 0.5f, 1e-5f));
        const SwayBinding past = swayChainBinding(chain, bones, 1.5f, 0.5f);
        assert(past.bone == 1 && past.weight == 1.0f);
        const SwayBinding justBefore = swayChainBinding(chain, bones, 1.999f, 0.5f);
        const SwayBinding justAfter = swayChainBinding(chain, bones, 2.001f, 0.5f);
        assert(justBefore.bone == 1 && justBefore.weight == 1.0f && justAfter.bone == 2 && justAfter.weight < 0.01f);
        assert(swayChainBinding(chain, bones, 6.0f, 0.5f).bone == 3 && swayChainBinding(chain, bones, 9.0f, 0.5f).bone == 3);
        assert(swayChainBinding(chain, bones, -1.0f, 0.5f).bone == 1 && swayChainBinding(chain, bones, -1.0f, 0.5f).weight == 0.0f);

        // A root chain's first bone has no parent: its own weight is whole.
        std::vector<SwayBone> rooted;
        const SwayChain trunk = appendSwayChain(rooted, {Vec3{}, Vec3{0.0f, 4.0f, 0.0f}}, {0.1f, 0.1f}, -1, 0, 2.0f);
        assert(swayChainBinding(trunk, rooted, 0.0f, 0.5f).weight == 1.0f && swayChainBinding(trunk, rooted, 2.0f, 0.5f).weight == 0.0f);
    }

    // Parents before their children, or not ordered.
    void testTheOrder() {
        std::vector<SwayBone> bones(3);
        bones[0].parent = -1;
        bones[1].parent = 0;
        bones[2].parent = 1;
        assert(swayBonesOrdered(bones));
        bones[1].parent = 2;
        assert(!swayBonesOrdered(bones));
        bones[1].parent = 1;
        assert(!swayBonesOrdered(bones));
        bones[1].parent = -1;
        assert(swayBonesOrdered(bones));
    }
}

void testSwayBones() {
    testAChainIsCut();
    testTheArcOfAPoint();
    testTheBindingAlongAChain();
    testTheOrder();
}
