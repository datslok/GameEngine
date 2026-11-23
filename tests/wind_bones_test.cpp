#include "render/gpu/wind_bones.h"
#include "scene/sway_bones.h"
#include "math/mat4.h"
#include "math/transform.h"

#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <random>
#include <stdexcept>
#include <vector>

namespace {
    constexpr float twoPi = 6.28318530718f;

    // No swing along or across the wind: only the steady lean.
    WindBoneMotion steadyMotion() {
        WindBoneMotion motion;
        motion.swingShare = 0.0f;
        motion.sideShare = 0.0f;
        return motion;
    }

    bool throwsInvalid(const WindBoneMotion& motion) {
        try {
            validateWindBoneMotion(motion);
        } catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    // A straight trunk height metres up, about 2 m a bone, radius at its foot falling to tipRadius at its top.
    std::vector<SwayBone> trunk(float height, float footRadius, float tipRadius) {
        std::vector<SwayBone> bones;
        appendSwayChain(bones, {Vec3{}, Vec3{0.0f, height, 0.0f}}, {footRadius, tipRadius}, -1, 0, 2.0f);
        return bones;
    }

    // A tapering 12 m trunk with two branches off it (one along x, one rising along z) and a twig chain off the first.
    std::vector<SwayBone> branchedTree() {
        std::vector<SwayBone> bones = trunk(12.0f, 0.3f, 0.08f);
        const int fourth = 3;
        const Vec3 fork = bones[fourth].tip;
        const int branch = appendSwayChain(bones, {fork, fork + Vec3{5.0f, 0.5f, 0.0f}}, {0.08f, 0.02f}, fourth, 1, 2.0f).firstBone;
        appendSwayChain(bones, {fork, fork + Vec3{0.0f, 3.0f, 3.0f}}, {0.07f, 0.02f}, fourth, 1, 2.0f);
        const SwayBone& last = bones[static_cast<std::size_t>(branch + 1)];
        appendSwayChain(bones, {last.tip, last.tip + Vec3{0.3f, 1.2f, 0.4f}}, {0.015f, 0.005f}, branch + 1, 2, 2.0f);
        return bones;
    }

    // Each bone's parent's pose, the rest pose for a root.
    WindBonePose parentPose(const std::vector<SwayBone>& bones, const std::vector<WindBonePose>& poses, int bone) {
        const int parent = bones[static_cast<std::size_t>(bone)].parent;
        return parent < 0 ? WindBonePose{} : poses[static_cast<std::size_t>(parent)];
    }

    // In still air every bone keeps its rest pose: nothing moves, at any time.
    void testStillAirIsStill() {
        const std::vector<SwayBone> bones = branchedTree();

        for (float seconds : {0.0f, 1.7f, 93.2f}) {
            const std::vector<WindBonePose> poses = windTreePoses(bones, 12.0f, 1.0f, 7u, Vec2{0.0f, 0.0f}, seconds, WindBoneMotion{});

            for (std::size_t k = 0; k < bones.size(); ++k) {
                assert((windBonePlace(poses[k], bones[k].tip) - bones[k].tip).length() < 1e-6f);
            }
        }
    }

    /*
    * In any wind the foot stays, each bone keeps its length, and each child's pivot is where its parent carries it (a
    * joint never opens): a vertex at the joint lands on the same place by either bone.
    */
    void testJointsHold() {
        const std::vector<SwayBone> bones = branchedTree();
        const std::vector<WindBonePose> poses = windTreePoses(bones, 12.0f, 1.0f, 3u, Vec2{0.2f, -0.15f}, 4.3f, WindBoneMotion{});
        assert((windBonePlace(poses[0], bones[0].pivot) - bones[0].pivot).length() < 1e-6f);
        bool somethingMoved = false;

        for (std::size_t k = 0; k < bones.size(); ++k) {
            const Vec3 pivot = windBonePlace(poses[k], bones[k].pivot);
            const Vec3 tip = windBonePlace(poses[k], bones[k].tip);
            assert(std::abs((tip - pivot).length() - (bones[k].tip - bones[k].pivot).length()) < 1e-4f);
            assert((pivot - windBonePlace(parentPose(bones, poses, static_cast<int>(k)), bones[k].pivot)).length() < 1e-4f);
            somethingMoved = somethingMoved || (tip - bones[k].tip).length() > 0.01f;
        }

        assert(somethingMoved);
    }

    /*
    * An untapered straight trunk in a steady lean bends as today's sway does: its tip moves downwind by about lean
    * times its height (within the bones' steps, (n + 1) / n for n bones), not sideways; the other way round for a wind
    * along z.
    */
    void testATrunkBendsAsTheTreesDo() {
        const std::vector<SwayBone> bones = trunk(20.0f, 0.3f, 0.3f);
        assert(bones.size() == 10);
        const Vec3 top = bones.back().tip;
        const float lean = 0.05f;

        const std::vector<WindBonePose> alongX = windTreePoses(bones, 20.0f, 1.0f, 1u, Vec2{lean, 0.0f}, 0.0f, steadyMotion());
        const Vec3 movedX = windBonePlace(alongX.back(), top) - top;
        assert(movedX.x > lean * 20.0f && movedX.x < lean * 20.0f * 1.12f);
        assert(std::abs(movedX.z) < 1e-4f);

        const std::vector<WindBonePose> alongZ = windTreePoses(bones, 20.0f, 1.0f, 1u, Vec2{0.0f, -lean}, 0.0f, steadyMotion());
        const Vec3 movedZ = windBonePlace(alongZ.back(), top) - top;
        assert(movedZ.z < -lean * 20.0f && std::abs(movedZ.x) < 1e-4f);
    }

    /*
    * A bone's share of the lean: 2 L / H for the trunk's own radius, growing as (foot / r)^exponent for thinner bones,
    * held under most; an exponent of 0 makes every bone alike.
    */
    void testThinnerBonesTurnMore() {
        assert(near(windBoneShare(2.0f, 20.0f, 0.3f, 0.3f, 3.0f, 8.0f), 0.2f, 1e-6f));
        assert(near(windBoneShare(2.0f, 20.0f, 0.15f, 0.3f, 3.0f, 8.0f), 1.6f, 1e-5f));
        assert(near(windBoneShare(2.0f, 20.0f, 0.2f, 0.3f, 3.0f, 8.0f), 0.2f * 3.375f, 1e-5f));
        assert(near(windBoneShare(2.0f, 20.0f, 0.01f, 0.3f, 3.0f, 8.0f), 1.6f, 1e-5f));
        assert(near(windBoneShare(2.0f, 20.0f, 0.01f, 0.3f, 0.0f, 8.0f), 0.2f, 1e-6f));
        assert(near(windBoneShare(1.0f, 20.0f, 0.3f, 0.3f, 3.0f, 8.0f), 0.1f, 1e-6f));
    }

    /*
    * A branch swings at a cantilever's first mode: a 30 m trunk 0.3 m thick about 0.28 Hz (trees measure 0.3-0.5), a
    * 4 m branch 5 cm thick about 2.6 Hz; thinner at one length slower, shorter faster; held within the range.
    */
    void testBranchesSwingAtTheirMode() {
        assert(near(windBranchHertz(30.0f, 0.3f, 3000.0f, 0.01f, 100.0f), 0.2798f, 0.002f));
        assert(near(windBranchHertz(4.0f, 0.05f, 3000.0f, 0.01f, 100.0f), 2.623f, 0.01f));
        assert(windBranchHertz(4.0f, 0.03f, 3000.0f, 0.01f, 100.0f) < windBranchHertz(4.0f, 0.05f, 3000.0f, 0.01f, 100.0f));
        assert(windBranchHertz(2.0f, 0.05f, 3000.0f, 0.01f, 100.0f) > windBranchHertz(4.0f, 0.05f, 3000.0f, 0.01f, 100.0f));
        assert(near(windBranchHertz(0.5f, 0.01f, 3000.0f, 0.2f, 3.0f), 3.0f, 1e-6f));
        assert(near(windBranchHertz(60.0f, 0.1f, 3000.0f, 0.2f, 3.0f), 0.2f, 1e-6f));
    }

    // A branch's phase: the same for the same tree and chain, within a turn, and different between chains and trees.
    void testBranchPhases() {
        assert(windBranchPhase(5u, 12u) == windBranchPhase(5u, 12u));
        assert(windBranchPhase(5u, 12u) != windBranchPhase(5u, 13u) && windBranchPhase(5u, 12u) != windBranchPhase(6u, 12u));

        for (std::uint32_t chain = 0; chain < 200; ++chain) {
            const float phase = windBranchPhase(17u, chain);
            assert(phase >= 0.0f && phase < twoPi);
        }
    }

    /*
    * A bone turns by its share of the wind across it: one pointing along the wind does not turn; one across it turns
    * towards it; the swing along the wind scales the turn by 1 + swing sin t; the swing across turns a horizontal branch
    * across the wind up or down (it bobs), and nothing without it.
    */
    void testTheTurnFollowsTheWind() {
        const Vec2 lean{0.0f, 0.1f};
        const Vec3 alongWind = windBoneTurn(Vec3{0.0f, 0.0f, 1.0f}, 1.0f, lean, 1.0f, 0.3f, 2.0f, Vec2{0.3f, 0.3f});
        assert(alongWind.length() < 1e-6f);

        const Vec3 across{1.0f, 0.0f, 0.0f};
        const Vec3 steady = windBoneTurn(across, 0.5f, lean, 1.0f, 0.0f, 0.0f, Vec2{0.0f, 0.0f});
        assert((steady - Vec3{0.0f, -0.05f, 0.0f}).length() < 1e-6f);
        assert(Quat::fromRotationVector(steady).rotate(across).z > 0.0f);

        // At a quarter turn sin t is 1 and cos t is 0: the swing along the wind at its height, nothing across.
        const Vec3 swung = windBoneTurn(across, 0.5f, lean, 1.0f, twoPi * 0.25f, 0.0f, Vec2{0.4f, 0.3f});
        assert((swung - steady * 1.4f).length() < 1e-6f);

        // At t 0 the swing across is at its height: the branch bobs (its tip leaves the ground's plane).
        const Vec3 bobbing = windBoneTurn(across, 0.5f, lean, 1.0f, 0.0f, 0.0f, Vec2{0.0f, 0.3f});
        assert(std::abs(Quat::fromRotationVector(bobbing).rotate(across).y) > 0.01f);
        assert(std::abs(Quat::fromRotationVector(steady).rotate(across).y) < 1e-6f);

        // A trunk sways across the wind too.
        const Vec3 up{0.0f, 1.0f, 0.0f};
        assert(std::abs(Quat::fromRotationVector(windBoneTurn(up, 0.5f, lean, 1.0f, 0.0f, 0.0f, Vec2{0.0f, 0.3f})).rotate(up).x) > 0.01f);
        assert(std::abs(Quat::fromRotationVector(windBoneTurn(up, 0.5f, lean, 1.0f, 0.0f, 0.0f, Vec2{0.0f, 0.0f})).rotate(up).x) < 1e-6f);
    }

    // A blended place lies between its two bones' images (all its own at weight 1); a blended direction is unit.
    void testBlending() {
        const WindBonePose rest{};
        const WindBonePose turned = windBonePose(rest, Vec3{0.0f, 0.0f, 0.3f}, Vec3{0.0f, 1.0f, 0.0f});
        const Vec3 place{0.5f, 2.0f, 0.1f};
        const Vec3 own = windBonePlace(turned, place);
        assert((windBlendedPlace(turned, rest, 1.0f, place) - own).length() < 1e-6f);
        assert((windBlendedPlace(turned, rest, 0.0f, place) - place).length() < 1e-6f);
        assert((windBlendedPlace(turned, rest, 0.25f, place) - (own * 0.25f + place * 0.75f)).length() < 1e-6f);

        const Vec3 normal = windBlendedDirection(turned, rest, 0.5f, Vec3{1.0f, 0.0f, 0.0f});
        assert(near(normal.length(), 1.0f, 1e-5f) && normal.y > 0.0f && normal.y < turned.turn.rotate(Vec3{1.0f, 0.0f, 0.0f}).y);
    }

    /*
    * A twig's rigid pose (windRigidPose): its foot lands where a vertex bound there blends to (the bark it grows from),
    * its turn the two turns blended along the short way and unit; all its own bone's at weight 1, its parent's at 0. A
    * turn given as its negative (the same rotation) blends alike. Every other point keeps its distance from the foot.
    */
    void testRigidPose() {
        const WindBonePose rest{};
        const WindBonePose parent = windBonePose(rest, Vec3{0.1f, 0.0f, -0.05f}, Vec3{});
        const WindBonePose own = windBonePose(parent, Vec3{0.0f, 0.2f, 0.25f}, Vec3{0.0f, 3.0f, 0.0f});
        const Vec3 foot{0.4f, 3.6f, -0.2f};
        const Vec3 leaf = foot + Vec3{0.3f, 0.1f, 0.25f};

        for (const float weight : {0.0f, 0.3f, 1.0f}) {
            const WindBonePose rigid = windRigidPose(own, parent, weight, foot);
            assert((windBonePlace(rigid, foot) - windBlendedPlace(own, parent, weight, foot)).length() < 1e-5f);
            assert(near(rigid.turn.length(), 1.0f, 1e-5f));
            assert(near((windBonePlace(rigid, leaf) - windBonePlace(rigid, foot)).length(), (leaf - foot).length(), 1e-5f));
        }

        const WindBonePose all = windRigidPose(own, parent, 1.0f, foot);
        assert((windBonePlace(all, leaf) - windBonePlace(own, leaf)).length() < 1e-5f);
        const WindBonePose none = windRigidPose(own, parent, 0.0f, foot);
        assert((windBonePlace(none, leaf) - windBonePlace(parent, leaf)).length() < 1e-5f);

        const WindBonePose flipped{.turn = -own.turn, .shift = own.shift};
        const WindBonePose half = windRigidPose(own, parent, 0.5f, foot);
        const WindBonePose halfFlipped = windRigidPose(flipped, parent, 0.5f, foot);
        assert((windBonePlace(half, leaf) - windBonePlace(halfFlipped, leaf)).length() < 1e-5f);
        const Vec3 between = windBonePlace(half, leaf);
        assert((between - windBonePlace(own, leaf)).length() < (windBonePlace(parent, leaf) - windBonePlace(own, leaf)).length());
    }

    /*
    * A world point on a placed plant's bones (a liana's hold on its tree, windPlantPointOffset) moves exactly as the
    * plant's own vertex there does (the model turning and scaling the blended move in the mesh's frame, as
    * boneSwayOfVertex does), and not at all while the bones rest.
    */
    void testAPointOnAPlantMoves() {
        const WindBonePose rest{};
        const WindBonePose parent = windBonePose(rest, Vec3{0.1f, 0.0f, -0.05f}, Vec3{});
        const WindBonePose own = windBonePose(parent, Vec3{0.0f, 0.2f, 0.25f}, Vec3{0.0f, 3.0f, 0.0f});
        const Vec3 foot{120.0f, 4.0f, -35.0f};

        for (const float turn : {0.0f, 0.9f, -2.2f}) {
            const Transform placed{.position = foot, .rotation = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, turn), .scale = Vec3{1.4f, 1.4f, 1.4f}};
            const Mat4 model = placed.getMatrix();
            const Vec3 inMesh{0.6f, 3.4f, -0.3f};
            const Vec3 world = transformPoint(model, inMesh);
            const Vec3 expected = transformDirection(model, windBlendedPlace(own, parent, 0.4f, inMesh) - inMesh);
            assert((windPlantPointOffset(own, parent, 0.4f, world, foot, 1.4f, turn) - expected).length() < 1e-4f);
            assert(windPlantPointOffset(rest, rest, 0.4f, world, foot, 1.4f, turn).length() < 1e-5f);
        }
    }

    // The union of two spheres holds both: the larger one when it holds the other, else just reaching both.
    void testSphereUnion() {
        const Vec4 big{0.0f, 0.0f, 0.0f, 5.0f};
        const Vec4 inside{1.0f, 1.0f, 0.0f, 1.0f};
        const Vec4 kept = windSphereUnion(big, inside);
        const Vec4 keptOther = windSphereUnion(inside, big);
        assert(kept.x == 0.0f && kept.w == 5.0f && keptOther.x == 0.0f && keptOther.w == 5.0f);

        const Vec4 apart = windSphereUnion(Vec4{0.0f, 0.0f, 0.0f, 1.0f}, Vec4{10.0f, 0.0f, 0.0f, 2.0f});
        assert(near(apart.w, 6.5f, 1e-5f) && near(apart.x, 5.5f, 1e-5f) && std::abs(apart.y) < 1e-6f);
    }

    /*
    * A cluster's bound holds over random poses: points along the tree's chains, each bound as swayChainBinding binds
    * it, gathered by runs of bones; their rest sphere blended by each bone of the run and its parent (windBlendedSphere),
    * merged, holds every point's blended place in random winds and times, with no pad.
    */
    void testClusterBoundsHold() {
        std::vector<SwayBone> bones;
        const SwayChain stem = appendSwayChain(bones, {Vec3{}, Vec3{0.3f, 9.0f, 0.0f}}, {0.25f, 0.1f}, -1, 0, 2.0f);
        const SwayChain bough = appendSwayChain(bones, {bones[2].tip, bones[2].tip + Vec3{6.0f, 1.0f, -2.0f}}, {0.08f, 0.01f}, 2, 1, 2.0f);
        std::mt19937 random{29u};
        std::uniform_real_distribution<float> unit{-1.0f, 1.0f};
        std::uniform_real_distribution<float> share{0.0f, 1.0f};

        for (const SwayChain& chain : {stem, bough}) {
            const Vec3 start = bones[static_cast<std::size_t>(chain.firstBone)].pivot;
            const Vec3 end = bones[static_cast<std::size_t>(chain.firstBone + chain.boneCount - 1)].tip;

            // Clusters of points within two neighbouring bones' stretch, scattered 30 cm round the line.
            for (int first = 0; first + 1 < chain.boneCount; ++first) {
                std::vector<Vec3> points;
                std::vector<SwayBinding> bindings;

                for (int k = 0; k < 40; ++k) {
                    const float arc = (static_cast<float>(first) + 2.0f * share(random)) * chain.length / static_cast<float>(chain.boneCount);
                    points.push_back(start + (end - start) * (arc / chain.length) + Vec3{unit(random), unit(random), unit(random)} * 0.3f);
                    bindings.push_back(swayChainBinding(chain, bones, arc, 0.5f));
                }

                Vec4 rest{points[0].x, points[0].y, points[0].z, 0.0f};

                for (const Vec3& point : points) {
                    rest = windSphereUnion(rest, Vec4{point.x, point.y, point.z, 0.0f});
                }

                for (int trial = 0; trial < 50; ++trial) {
                    const Vec2 lean{unit(random) * 0.4f, unit(random) * 0.4f};
                    const std::vector<WindBonePose> poses = windTreePoses(bones, 9.0f, 1.0f, 11u, lean, share(random) * 100.0f, WindBoneMotion{});
                    const int firstBone = chain.firstBone + first;
                    Vec4 bound = windBlendedSphere(poses[static_cast<std::size_t>(firstBone)], parentPose(bones, poses, firstBone), rest);
                    bound = windSphereUnion(bound, windBlendedSphere(poses[static_cast<std::size_t>(firstBone + 1)], parentPose(bones, poses, firstBone + 1), rest));

                    for (std::size_t k = 0; k < points.size(); ++k) {
                        const int bone = bindings[k].bone;
                        const Vec3 posed = windBlendedPlace(poses[static_cast<std::size_t>(bone)], parentPose(bones, poses, bone), bindings[k].weight, points[k]);
                        assert((posed - Vec3{bound.x, bound.y, bound.z}).length() <= bound.w + 1e-4f);
                    }
                }
            }
        }
    }

    /*
    * A tree's move (ROADMAP phase 21 stage 3c-iv-a): a pose moves nothing at rest; a turn about the pivot keeps it and
    * swings the tip by 2 sin(angle / 2) of the bone; a shift moves both by it. Over random winds every point near the
    * tree's bones, blended between its bone and that bone's parent or carried rigidly at a foot as a twig, moves no
    * farther than its bones' largest moves give for the farthest point's reach.
    */
    void testATreesMoveBoundsItsPoints() {
        const WindBonePose rest{};
        const Vec2 still = windPoseMoves(rest, Vec3{1.0f, 2.0f, 3.0f}, Vec3{1.0f, 4.0f, 3.0f});
        assert(still.x == 0.0f && still.y == 0.0f);
        const Vec3 pivot{1.0f, 2.0f, 3.0f};
        const float angle = 0.3f;
        const WindBonePose turned = windBonePose(rest, Vec3{0.0f, 0.0f, angle}, pivot);
        const Vec2 turnMoves = windPoseMoves(turned, pivot, pivot + Vec3{0.0f, 2.0f, 0.0f});
        assert(near(turnMoves.x, 2.0f * 2.0f * std::sin(angle * 0.5f), 1e-5f) && near(turnMoves.y, 2.0f * std::sin(angle * 0.5f), 1e-6f));
        const WindBonePose shifted{.turn = Quat{}, .shift = Vec3{0.0f, 0.0f, 0.25f}};
        const Vec2 shiftMoves = windPoseMoves(shifted, pivot, pivot + Vec3{0.0f, 2.0f, 0.0f});
        assert(near(shiftMoves.x, 0.25f, 1e-6f) && near(shiftMoves.y, 0.0f, 1e-6f));
        assert(near(windTreeMove(Vec2{0.5f, 0.1f}, 2.0f), 0.7f, 1e-6f));

        const std::vector<SwayBone> bones = branchedTree();
        std::mt19937 random{41u};
        std::uniform_real_distribution<float> unit{-1.0f, 1.0f};
        std::uniform_real_distribution<float> share{0.0f, 1.0f};
        std::uniform_int_distribution<int> anyBone{0, static_cast<int>(bones.size()) - 1};
        std::vector<Vec3> points;
        std::vector<SwayBinding> bindings;
        std::vector<Vec3> twigTips;
        float reach = 0.0f;

        for (int k = 0; k < 400; ++k) {
            const int bone = anyBone(random);
            const SwayBone& b = bones[static_cast<std::size_t>(bone)];
            points.push_back(b.pivot + (b.tip - b.pivot) * share(random) + Vec3{unit(random), unit(random), unit(random)} * 0.4f);
            bindings.push_back(SwayBinding{.bone = bone, .weight = share(random)});
            twigTips.push_back(points.back() + Vec3{unit(random), unit(random), unit(random)} * 0.6f);
            reach = std::max(reach, swayBindingReach(bones, points.back(), bindings.back()) + (twigTips.back() - points.back()).length());
        }

        for (int trial = 0; trial < 60; ++trial) {
            const Vec2 lean{unit(random) * 0.5f, unit(random) * 0.5f};
            const std::vector<WindBonePose> poses = windTreePoses(bones, 12.0f, 1.0f, 5u, lean, share(random) * 100.0f, WindBoneMotion{});
            Vec2 largest{0.0f, 0.0f};

            for (std::size_t bone = 0; bone < bones.size(); ++bone) {
                const Vec2 moves = windPoseMoves(poses[bone], bones[bone].pivot, bones[bone].tip);
                largest = Vec2{std::max(largest.x, moves.x), std::max(largest.y, moves.y)};
            }

            const float move = windTreeMove(largest, reach);
            float farthest = 0.0f;

            for (std::size_t k = 0; k < points.size(); ++k) {
                const int bone = bindings[k].bone;
                const WindBonePose& own = poses[static_cast<std::size_t>(bone)];
                const WindBonePose parent = parentPose(bones, poses, bone);
                const float blended = (windBlendedPlace(own, parent, bindings[k].weight, points[k]) - points[k]).length();
                const float twig = (windBonePlace(windRigidPose(own, parent, bindings[k].weight, points[k]), twigTips[k]) - twigTips[k]).length();
                assert(blended <= move + 1e-4f && twig <= move + 1e-4f);
                farthest = std::max({farthest, blended, twig});
            }

            // Not padded past use: within a few times the farthest move seen.
            assert(lean.length() < 0.1f || move < 4.0f * farthest + 1e-3f);
        }
    }

    // The settings refuse what makes no sense.
    void testMotionIsValidated() {
        validateWindBoneMotion(WindBoneMotion{});
        WindBoneMotion motion;
        motion.stiffnessExponent = -1.0f;
        assert(throwsInvalid(motion));
        motion = WindBoneMotion{};
        motion.mostStiffness = 0.5f;
        assert(throwsInvalid(motion));
        motion = WindBoneMotion{};
        motion.woodSpeed = 0.0f;
        assert(throwsInvalid(motion));
        motion = WindBoneMotion{};
        motion.slowestHertz = 4.0f;
        assert(throwsInvalid(motion));
        motion = WindBoneMotion{};
        motion.slowestHertz = 0.0f;
        assert(throwsInvalid(motion));
        motion = WindBoneMotion{};
        motion.sideShare = -0.1f;
        assert(throwsInvalid(motion));
        motion = WindBoneMotion{};
        motion.poolMostBones = motion.poolBones - 1;
        assert(throwsInvalid(motion));
        motion = WindBoneMotion{};
        motion.poolMostBones = (1 << 24) + 1;
        assert(throwsInvalid(motion));
    }
}

void testWindBones() {
    testStillAirIsStill();
    testJointsHold();
    testATrunkBendsAsTheTreesDo();
    testThinnerBonesTurnMore();
    testBranchesSwingAtTheirMode();
    testBranchPhases();
    testTheTurnFollowsTheWind();
    testBlending();
    testRigidPose();
    testAPointOnAPlantMoves();
    testSphereUnion();
    testClusterBoundsHold();
    testATreesMoveBoundsItsPoints();
    testMotionIsValidated();
}
