#include "scene/indexed_mesh.h"
#include "scene/simplify_mesh.h"
#include "scene/sway_bones.h"
#include "math/quat.h"

#include "mesh_helpers.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <stdexcept>
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
            assert(bone.chainStart == 1 && near(bone.chainLength, 6.0f, 1e-5f) && near(bone.chainRadius, 0.4f, 1e-6f));
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

    /*
    * A point near a skeleton follows its nearest line where its closest point lies along it: by the trunk the trunk's
    * bone, by the branch the branch's (blended towards the trunk near the fork), whichever line comes first in the list.
    * A line's sphere holds its points.
    */
    void testTheNearestBinding() {
        std::vector<SwayBone> bones;
        const std::vector<Vec3> trunkLine{Vec3{}, Vec3{0.0f, 8.0f, 0.0f}};
        const std::vector<Vec3> branchLine{Vec3{0.0f, 4.0f, 0.0f}, Vec3{4.0f, 6.0f, 0.0f}};
        const SwayChain trunk = appendSwayChain(bones, trunkLine, {0.3f, 0.1f}, -1, 0, 2.0f);
        const SwayChain branch = appendSwayChain(bones, branchLine, {0.1f, 0.02f}, 1, 1, 2.0f);
        const SwayLine trunkSway = makeSwayLine(trunk, trunkLine);
        const SwayLine branchSway = makeSwayLine(branch, branchLine);
        assert(near(trunkSway.radius, 4.0f, 1e-5f) && (trunkSway.centre - Vec3{0.0f, 4.0f, 0.0f}).length() < 1e-5f);

        for (const std::vector<SwayLine>& lines : {std::vector<SwayLine>{trunkSway, branchSway}, std::vector<SwayLine>{branchSway, trunkSway}}) {
            const SwayBinding low = swayNearestBinding(lines, bones, Vec3{0.2f, 1.0f, 0.1f}, 0.5f);
            assert(low.bone == 0 && low.weight == 1.0f);
            const SwayBinding high = swayNearestBinding(lines, bones, Vec3{-0.3f, 7.0f, 0.0f}, 0.5f);
            assert(high.bone == trunk.firstBone + 3 && near(high.weight, 1.0f, 1e-5f));
            const SwayBinding out = swayNearestBinding(lines, bones, Vec3{3.6f, 6.1f, 0.2f}, 0.5f);
            assert(out.bone == branch.firstBone + branch.boneCount - 1);
            const SwayBinding fork = swayNearestBinding(lines, bones, Vec3{0.3f, 4.2f, 0.0f}, 0.5f);
            assert(fork.bone == branch.firstBone && fork.weight < 0.5f);
        }

        bool threw = false;

        try {
            swayNearestBinding({}, bones, Vec3{}, 0.5f);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
    }

    /*
    * A world point in a plant's mesh frame: the placement (foot, turn about up, scale) undone, so placing it again
    * gives the point back; a quarter turn takes the mesh's x to the world's -z.
    */
    void testAPointInItsPlant() {
        const Vec3 foot{10.0f, 2.0f, -4.0f};
        const Vec3 local = swayPlantPoint(Vec3{10.0f, 5.0f, -6.0f}, foot, 2.0f, 1.5707963f);
        assert((local - Vec3{1.0f, 1.5f, 0.0f}).length() < 1e-5f);

        for (const float turn : {0.0f, 0.7f, -2.4f}) {
            const Vec3 point{3.0f, 7.5f, 1.25f};
            const Vec3 inPlant = swayPlantPoint(point, foot, 1.3f, turn);
            const Vec3 placed = foot + Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, turn).rotate(inPlant * 1.3f);
            assert((placed - point).length() < 1e-5f);
        }
    }

    // A mesh's bindings ride along: its prefix keeps its first ones, its simplified level each kept vertex's own.
    void testMeshesKeepTheirBindings() {
        IndexedMesh sheet = rippledSheet(12);

        for (std::size_t vertex = 0; vertex < sheet.vertices.size(); ++vertex) {
            sheet.swayBindings.push_back(SwayBinding{.bone = static_cast<int>(vertex), .weight = 0.5f});
        }

        const IndexedMesh prefix = meshPrefix(sheet, 30, 0);
        assert(prefix.swayBindings.size() == 30 && prefix.swayBindings[29].bone == 29);

        const IndexedMesh simpler = simplifyMesh(sheet, 0.2f, 1.0f, false);
        assert(simpler.vertices.size() < sheet.vertices.size() && simpler.swayBindings.size() == simpler.vertices.size());

        for (std::size_t vertex = 0; vertex < simpler.vertices.size(); ++vertex) {
            const MeshVertex& kept = sheet.vertices[static_cast<std::size_t>(simpler.swayBindings[vertex].bone)];
            assert((kept.position - simpler.vertices[vertex].position).lengthSquared() == 0.0f);
        }

        // Nothing bound, nothing carried.
        sheet.swayBindings.clear();
        assert(simplifyMesh(sheet, 0.2f, 1.0f, false).swayBindings.empty() && meshPrefix(sheet, 30, 0).swayBindings.empty());
    }
}

    /*
    * How far a bound point lies from its bones (ROADMAP phase 21 stage 3c-iv-a): from its own bone's segment (beside
    * it, past its tip, before its pivot), and from its parent's too while it blends with it; a root has no parent to
    * blend with. A mesh's reach is its farthest bound vertex's, none without bindings.
    */
    void testTheReachOfABinding() {
        std::vector<SwayBone> bones{SwayBone{.pivot = Vec3{}, .tip = Vec3{0.0f, 2.0f, 0.0f}}};
        bones.push_back(SwayBone{.pivot = Vec3{0.0f, 2.0f, 0.0f}, .tip = Vec3{3.0f, 2.0f, 0.0f}, .parent = 0, .level = 1});
        assert(near(swayBindingReach(bones, Vec3{0.5f, 1.0f, 0.0f}, SwayBinding{.bone = 0, .weight = 1.0f}), 0.5f, 1e-6f));
        assert(near(swayBindingReach(bones, Vec3{0.0f, 3.0f, 0.0f}, SwayBinding{.bone = 0, .weight = 1.0f}), 1.0f, 1e-6f));
        assert(near(swayBindingReach(bones, Vec3{0.0f, -0.5f, 0.0f}, SwayBinding{.bone = 0, .weight = 0.5f}), 0.5f, 1e-6f));
        assert(near(swayBindingReach(bones, Vec3{2.0f, 2.25f, 0.0f}, SwayBinding{.bone = 1, .weight = 1.0f}), 0.25f, 1e-6f));
        assert(near(swayBindingReach(bones, Vec3{2.0f, 2.25f, 0.0f}, SwayBinding{.bone = 1, .weight = 0.75f}), std::sqrt(4.0f + 0.0625f), 1e-5f));

        IndexedMesh mesh;
        mesh.vertices = {MeshVertex{Vec3{0.5f, 1.0f, 0.0f}, Vec3{}, Vec2{}, Vec4{}}, MeshVertex{Vec3{2.0f, 2.25f, 0.0f}, Vec3{}, Vec2{}, Vec4{}}};
        assert(swayMeshReach(bones, mesh) == 0.0f);
        mesh.swayBindings = {SwayBinding{.bone = 0, .weight = 1.0f}, SwayBinding{.bone = 1, .weight = 0.75f}};
        assert(near(swayMeshReach(bones, mesh), std::sqrt(4.0f + 0.0625f), 1e-5f));
    }

void testSwayBones() {
    testAChainIsCut();
    testTheArcOfAPoint();
    testTheBindingAlongAChain();
    testTheOrder();
    testTheNearestBinding();
    testAPointInItsPlant();
    testMeshesKeepTheirBindings();
    testTheReachOfABinding();
}
