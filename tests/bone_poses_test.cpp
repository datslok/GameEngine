#include "engine/debug_views.h"
#include "engine/engine_commands.h"
#include "math/quat.h"
#include "render/gpu/bone_poses.h"
#include "render/render_stats.h"
#include "core/command_registry.h"

#include "test_levels.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    /*
    * A trunk of three bones (level 0); a branch from its second bone and one from its third (level 1); a twig from the
    * first branch's end (level 2). Parents first, as treeBones lays them out: each chain after the bone it starts on.
    */
    SwaySkeleton threeLevelTree() {
        SwaySkeleton tree;
        appendSwayChain(tree.bones, {Vec3{}, Vec3{0.0f, 9.0f, 0.0f}}, {0.4f, 0.1f}, -1, 0, 3.0f);
        appendSwayChain(tree.bones, {Vec3{0.0f, 4.0f, 0.0f}, Vec3{4.0f, 6.0f, 1.0f}}, {0.12f, 0.03f}, 1, 1, 2.0f);
        const int branchEnd = static_cast<int>(tree.bones.size()) - 1;
        appendSwayChain(tree.bones, {Vec3{0.0f, 7.0f, 0.0f}, Vec3{-3.0f, 8.0f, -2.0f}}, {0.08f, 0.02f}, 2, 1, 2.0f);
        appendSwayChain(tree.bones, {Vec3{4.0f, 6.0f, 1.0f}, Vec3{5.0f, 7.5f, 1.5f}}, {0.03f, 0.01f}, branchEnd, 2, 1.0f);
        tree.height = 9.0f;
        return tree;
    }

    // Chains packed level by level: the trunk, then both branches, then the twig; every bone in exactly one chain, the
    // second skeleton's levels after the first's. A chain on a bone of its own level, or broken, is refused.
    void testChainsByLevel() {
        const SwaySkeleton tree = threeLevelTree();
        SwaySkeleton stick;
        appendSwayChain(stick.bones, {Vec3{}, Vec3{0.0f, 2.0f, 0.0f}}, {0.1f, 0.1f}, -1, 0, 1.0f);
        const SwaySkeletonTables tables = packSwaySkeletons({&tree, &stick});

        const GpuSwaySkeleton& first = tables.skeletons[0];
        assert(first.levelCount == 3 && first.firstLevel == 0);
        assert(tables.levelStarts[0] == 0 && tables.levelStarts[1] == 1 && tables.levelStarts[2] == 3 && tables.levelStarts[3] == 4);
        assert(tables.chains[0].firstBone == 0 && tables.chains[0].boneCount == 3);
        assert(tables.chains[1].firstBone == 3 && tables.chains[2].firstBone > tables.chains[1].firstBone);

        std::vector<int> covered(tree.bones.size(), 0);

        for (std::uint32_t chain = 0; chain < tables.levelStarts[first.firstLevel + first.levelCount]; ++chain) {
            for (std::uint32_t bone = 0; bone < tables.chains[chain].boneCount; ++bone) {
                ++covered.at(tables.chains[chain].firstBone + bone);
            }
        }

        assert(std::all_of(covered.begin(), covered.end(), [](int times) { return times == 1; }));

        const GpuSwaySkeleton& second = tables.skeletons[1];
        assert(second.firstLevel == 4 && second.levelCount == 1 && tables.levelStarts[5] == tables.levelStarts[4] + 1);
        assert(tables.chains[tables.levelStarts[4]].firstBone == 0 && second.leanPart == boneNoPart);

        SwaySkeleton sameLevel = tree;
        sameLevel.bones[3].level = 0;
        sameLevel.bones[4].level = 0;
        assert(throwsInvalid([&] { packSwaySkeletons({&sameLevel}); }));
        SwaySkeleton broken = tree;
        broken.bones[2].parent = 0;
        assert(throwsInvalid([&] { packSwaySkeletons({&broken}); }));
    }

    // The pass's order (level by level, a chain bone by bone) gives the list order's poses, for any scale; a larger
    // tree's branches swing slower, so its poses differ at a time.
    void testLevelPosesMatchListOrder() {
        const SwaySkeleton tree = threeLevelTree();
        const SwaySkeletonTables tables = packSwaySkeletons({&tree});
        const WindBoneMotion motion;
        const Vec2 lean{0.12f, -0.07f};

        for (const float scale : {0.6f, 1.0f, 1.7f}) {
            const WindTreeShape shape{.height = tree.height, .footRadius = tree.bones[0].radius, .metresPerUnit = scale, .seed = 41u};
            const std::vector<WindBonePose> levels = boneTreePoses(tables, 0, shape, lean, 3.7f, motion);
            const std::vector<WindBonePose> list = windTreePoses(tree.bones, tree.height, scale, 41u, lean, 3.7f, motion);
            assert(levels.size() == list.size());

            for (std::size_t bone = 0; bone < list.size(); ++bone) {
                assert(near(levels[bone].turn.x, list[bone].turn.x, 1e-6f) && near(levels[bone].turn.w, list[bone].turn.w, 1e-6f));
                assert(near(levels[bone].shift.x, list[bone].shift.x, 1e-5f) && near(levels[bone].shift.z, list[bone].shift.z, 1e-5f));
            }
        }

        const WindTreeShape small{.height = tree.height, .footRadius = 0.4f, .metresPerUnit = 0.5f, .seed = 41u};
        const WindTreeShape large{.height = tree.height, .footRadius = 0.4f, .metresPerUnit = 2.0f, .seed = 41u};
        const WindBonePose a = boneTreePoses(tables, 0, small, lean, 3.7f, motion).back();
        const WindBonePose b = boneTreePoses(tables, 0, large, lean, 3.7f, motion).back();
        assert(std::abs(a.turn.x - b.turn.x) + std::abs(a.turn.y - b.turn.y) + std::abs(a.turn.z - b.turn.z) > 1e-4f);
    }

    // A tree drawn at a scale turns as the same tree built that much larger at scale 1: its branches' hertz from metres.
    void testScaleIsMetres() {
        const SwaySkeleton tree = threeLevelTree();
        const float scale = 2.3f;
        std::vector<SwayBone> larger = tree.bones;

        for (SwayBone& bone : larger) {
            bone.pivot = bone.pivot * scale;
            bone.tip = bone.tip * scale;
            bone.radius *= scale;
            bone.chainLength *= scale;
            bone.chainRadius *= scale;
        }

        const Vec2 lean{0.09f, 0.14f};
        const std::vector<WindBonePose> drawn = windTreePoses(tree.bones, tree.height, scale, 5u, lean, 2.9f, WindBoneMotion{});
        const std::vector<WindBonePose> built = windTreePoses(larger, tree.height * scale, 1.0f, 5u, lean, 2.9f, WindBoneMotion{});

        for (std::size_t bone = 0; bone < drawn.size(); ++bone) {
            assert(near(drawn[bone].turn.x, built[bone].turn.x, 1e-5f) && near(drawn[bone].turn.y, built[bone].turn.y, 1e-5f));
            assert(near(drawn[bone].shift.z * scale, built[bone].shift.z, 1e-4f));
        }
    }

    // A lean along the ground turned into the plant's frame is turned back out by the plant's own turn.
    void testLeanInPlant() {
        for (const float turn : {0.0f, 0.7f, 2.5f, -1.9f}) {
            const Vec2 world{0.3f, -0.11f};
            const Vec2 own = windLeanInPlant(world, turn);
            const Vec3 back = Quat::fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, turn).rotate(Vec3{own.x, 0.0f, own.y});
            assert(near(back.x, world.x, 1e-6f) && near(back.z, world.y, 1e-6f) && near(back.y, 0.0f, 1e-6f));
        }
    }

    /*
    * Two parts: a bark part (skeleton 1, two level slots, groups 0 and 1) and a plain part (group 2); three views. The
    * swaying views 0 and 2 take plants their bark draws, each once and its bits off; not view 1's, the plain part's, or
    * a plant past the reach.
    */
    void testWantedPlants() {
        PlacedTable table;
        GpuPlacedPart bark;
        bark.firstGroup = 0;
        bark.levels.levels = 1;
        bark.unused[1] = 1;
        GpuPlacedPart plain;
        plain.firstGroup = 2;
        table.parts = {bark, plain};
        table.groups.resize(3);
        table.skeletons.resize(1);
        assert((boneGroups(table) == std::vector<std::uint32_t>{0, 1}));

        std::vector<GpuPlacedPlant> plants(8);
        plants[6].foot[0] = 500.0f;
        // Each view's groups' commands, every view's entries after each other.
        std::vector<GpuDrawCommand> commands(9);
        std::vector<GpuPlacedEntry> entries;
        const auto add = [&](std::uint32_t view, std::uint32_t group, std::vector<GpuPlacedEntry> list) {
            GpuDrawCommand& command = commands[view * 3 + group];
            command.firstInstance = placedInstanceBase + static_cast<std::uint32_t>(entries.size());
            command.instanceCount = static_cast<std::uint32_t>(list.size());
            entries.insert(entries.end(), list.begin(), list.end());
        };
        add(0, 0, {{3, 0}, {1 | DRAW_LIST_THINNED_BIT, 0}, {6, 0}});
        add(0, 1, {{3, 0}});
        add(0, 2, {{5, 1}});
        add(1, 0, {{4, 0}});
        add(2, 1, {{2, 0}, {1, 0}});

        const std::vector<std::uint32_t> swaying{0, 2};
        const std::vector<BoneWanted> wanted = boneWantedPlants(table, commands, entries, swaying, plants, Vec3{}, 400.0f);
        assert((wanted == std::vector<BoneWanted>{{1, 0}, {2, 0}, {3, 0}}));
        const std::vector<std::uint32_t> all{0, 1, 2};
        assert(boneWantedPlants(table, commands, entries, all, plants, Vec3{}, 1000.0f).size() == 5);
    }

    // Claims in order: room for each tree's bones (two poses a bone) until the pool is full; after that every tree
    // stands still, the bones asked still counted. A tree filling the pool exactly fits.
    void testClaims() {
        assert(boneTreeFirstPose(0, 10, 10) == 0 && boneTreeFirstPose(4, 6, 10) == 8 && boneTreeFirstPose(5, 6, 10) == boneNoPose);
        assert(boneTreeFirstPose(0xFFFFFFF0u, 100, 0xFFFFFFFFu) == boneNoPose);

        std::vector<GpuSwaySkeleton> skeletons(2);
        skeletons[0].boneCount = 40;
        skeletons[1].boneCount = 25;
        const std::vector<BoneWanted> wanted{{7, 0}, {2, 1}, {9, 0}, {4, 1}};
        const BoneClaims claims = claimBoneTrees(wanted, skeletons, 100);
        assert(claims.trees.size() == 4 && claims.bonesAsked == 130 && claims.still == 2);
        assert(claims.trees[0].firstPose == 0 && claims.trees[1].firstPose == 80 && claims.trees[1].plant == 2);
        assert(claims.trees[2].firstPose == boneNoPose && claims.trees[3].firstPose == boneNoPose);

        const std::vector<BoneWanted> unknown{{1, 5}};
        assert(throwsInvalid([&] { claimBoneTrees(unknown, skeletons, 100); }));
    }

    // The pool kept while the ask fits; past it, the next power of two at or above the ask, never above the cap (the
    // cap itself when the ask passes it) and never smaller than the pool already is.
    void testPoolGrows() {
        assert(boneGrownPool(262144, 0, 4194304) == 262144);
        assert(boneGrownPool(262144, 262144, 4194304) == 262144);
        assert(boneGrownPool(262144, 343540, 4194304) == 524288);
        assert(boneGrownPool(262144, 524289, 4194304) == 1048576);
        assert(boneGrownPool(100, 101, 4194304) == 128);
        assert(boneGrownPool(262144, 5000000, 4194304) == 4194304);
        assert(boneGrownPool(262144, 343540, 300000) == 300000);
        assert(boneGrownPool(524288, 300000, 4194304) == 524288);
        assert(boneGrownPool(4194304, 0xFFFFFFFFu, 16777216) == 16777216);
    }

    // A skeleton takes its lean from its first part swaying as a tree, not a climber on it; none without one.
    void testLeanParts() {
        PlacedTable table;
        table.parts.resize(4);
        table.parts[0].unused[1] = 2;
        table.parts[1].unused[1] = 1;
        table.parts[2].unused[1] = 1;
        table.parts[3].unused[1] = 0;
        table.draws = {3, 2, 1, 0};
        table.skeletons.resize(3);
        std::vector<DrawItem> drawn(4);
        drawn[3].sway.kind = SwayKind::Hanging;
        drawn[2].sway.kind = SwayKind::Hanging;
        drawn[1].sway.kind = SwayKind::Tree;
        drawn[0].sway.kind = SwayKind::Tree;
        const std::vector<std::uint32_t> lean = skeletonLeanParts(table, drawn);
        assert((lean == std::vector<std::uint32_t>{2, boneNoPart, boneNoPart}));
    }

    // Poses made as the pass makes them agree; a turned pose, a tree missing, one claimed twice or not wanted differ.
    void testComparison() {
        SwaySkeleton tree = threeLevelTree();
        tree.reach = 0.7f;
        const SwaySkeletonTables tables = packSwaySkeletons({&tree});
        std::vector<GpuPlacedPlant> plants(4);
        plants[1].scale = 1.3f;
        plants[1].pick = 0.37f;
        plants[3].scale = 0.8f;
        plants[3].pick = 0.81f;
        const std::vector<BoneWanted> wanted{{1, 0}, {3, 0}};
        BoneClaims claims = claimBoneTrees(wanted, tables.skeletons, 1000);
        claims.trees[0].lean[0] = 0.1f;
        claims.trees[0].previousLean[1] = -0.05f;
        claims.trees[1].lean[1] = 0.2f;
        const WindBoneMotion motion;
        std::vector<GpuBonePose> poses(2 * claims.bonesAsked);

        for (GpuBoneTree& posed : claims.trees) {
            const WindTreeShape shape = boneTreeShape(tables.skeletons[0], plants[posed.plant]);
            const std::vector<WindBonePose> now = boneTreePoses(tables, 0, shape, Vec2{posed.lean[0], posed.lean[1]}, 5.0f, motion);
            posed.move = boneTreeMove(tables, 0, now);
            const std::vector<WindBonePose> last = boneTreePoses(tables, 0, shape, Vec2{posed.previousLean[0], posed.previousLean[1]}, 4.98f, motion);

            for (std::size_t bone = 0; bone < now.size(); ++bone) {
                poses[posed.firstPose + 2 * bone] = packBonePose(now[bone], tree.bones[bone].parent);
                poses[posed.firstPose + 2 * bone + 1] = packBonePose(last[bone], tree.bones[bone].parent);
            }
        }

        const BoneComparison same = compareBonePoses(wanted, claims.trees, poses, tables, plants, motion, 5.0f, 4.98f);
        assert(same.posesChecked == 4 * tree.bones.size() && same.missing == 0 && same.extra == 0 && same.twice == 0);
        assert(describeBoneComparison(same).find("agree") != std::string::npos);

        std::vector<GpuBonePose> turned = poses;
        turned[2 * tree.bones.size() + 3].turn[1] += 0.01f;
        assert(describeBoneComparison(compareBonePoses(wanted, claims.trees, turned, tables, plants, motion, 5.0f, 4.98f)).find("DIFFER") !=
               std::string::npos);
        std::vector<GpuBonePose> shifted = poses;
        shifted[5].shift[2] += 0.01f;
        assert(compareBonePoses(wanted, claims.trees, shifted, tables, plants, motion, 5.0f, 4.98f).worstShift > 0.009f);
        assert(same.wrongParents == 0);
        std::vector<GpuBonePose> orphaned = poses;
        orphaned[2 * 4 + 1].parent = 0;
        const BoneComparison wrongParent = compareBonePoses(wanted, claims.trees, orphaned, tables, plants, motion, 5.0f, 4.98f);
        assert(wrongParent.wrongParents == 1 && describeBoneComparison(wrongParent).find("DIFFER") != std::string::npos);

        const std::vector<GpuBoneTree> one{claims.trees[0]};
        assert(compareBonePoses(wanted, one, poses, tables, plants, motion, 5.0f, 4.98f).missing == 1);
        const std::vector<GpuBoneTree> twice{claims.trees[0], claims.trees[1], claims.trees[1]};
        assert(compareBonePoses(wanted, twice, poses, tables, plants, motion, 5.0f, 4.98f).twice == 1);
        const std::vector<BoneWanted> fewer{{3, 0}};
        const BoneComparison extra = compareBonePoses(fewer, claims.trees, poses, tables, plants, motion, 5.0f, 4.98f);
        assert(extra.extra == 1 && describeBoneComparison(extra).find("DIFFER") != std::string::npos);

        std::vector<GpuBoneTree> moved = claims.trees;
        moved[1].move += 0.01f;
        const BoneComparison wrongMove = compareBonePoses(wanted, moved, poses, tables, plants, motion, 5.0f, 4.98f);
        assert(same.worstMove == 0.0f && wrongMove.worstMove > 0.009f && describeBoneComparison(wrongMove).find("DIFFER") != std::string::npos);
    }

    /*
    * A tree's move as the pose pass makes it (ROADMAP phase 21 stage 3c-iv-a): its bones' largest pivot or tip move and
    * largest turn, with its skeleton's reach (carried in its record); none at rest; more with a longer reach when turned.
    */
    void testTheTreesMove() {
        SwaySkeleton tree = threeLevelTree();
        tree.reach = 1.5f;
        const SwaySkeletonTables tables = packSwaySkeletons({&tree});
        assert(tables.skeletons[0].reach == 1.5f);
        const WindTreeShape shape{.height = 9.0f, .footRadius = 0.4f, .metresPerUnit = 1.0f, .seed = 3u};
        const std::vector<WindBonePose> poses = boneTreePoses(tables, 0, shape, Vec2{0.2f, -0.1f}, 2.5f, WindBoneMotion{});
        Vec2 largest{0.0f, 0.0f};

        for (std::size_t bone = 0; bone < tree.bones.size(); ++bone) {
            const Vec2 moves = windPoseMoves(poses[bone], tree.bones[bone].pivot, tree.bones[bone].tip);
            largest = Vec2{std::max(largest.x, moves.x), std::max(largest.y, moves.y)};
        }

        assert(largest.y > 0.0f && near(boneTreeMove(tables, 0, poses), windTreeMove(largest, 1.5f), 1e-6f));
        assert(boneTreeMove(tables, 0, std::vector<WindBonePose>(tree.bones.size())) == 0.0f);
    }

    // Each bones-compare call asks for a comparison and shows the last one; it takes no arguments.
    void testCompareCommand() {
        CommandRegistry registry;
        int asked = 0;
        std::string last;
        addBonesCompareCommand(registry, [&]() {
            ++asked;
            return last;
        });

        assert(!registry.execute("bones-compare now").ok && asked == 0);
        const CommandReply first = registry.execute("bones-compare");
        assert(first.ok && asked == 1 && first.text.find("No comparison") != std::string::npos);
        last = "Bones agree";
        const CommandReply second = registry.execute("bones-compare");
        assert(second.ok && asked == 2 && second.text.find("agree") != std::string::npos);
    }

    /*
    * What a vertex reads (3c-iii-c): a plant's tree only when claimed with this frame's stamp (none on a frame the pass
    * did not run, stamp 0, though the table starts cleared to 0; stamps wrap past 0); a bone's pose at its tree's first
    * pose + 2 b, last frame's after it.
    */
    void testClaimedTreeAndSlots() {
        assert(boneClaimedTree(5, 7, 5) == 7);
        assert(boneClaimedTree(4, 7, 5) == boneNoTree);
        assert(boneClaimedTree(0, 0, 0) == boneNoTree);
        assert(boneClaimedTree(5, boneNoTree, 5) == boneNoTree);
        assert(bonePoseSlot(10, 3, false) == 16 && bonePoseSlot(10, 3, true) == 17 && bonePoseSlot(0, 0, false) == 0);
        assert(boneNextStamp(0) == 1 && boneNextStamp(41) == 42 && boneNextStamp(0xFFFFFFFFu) == 1);
    }

    /*
    * A bound vertex's two poses from the pool: its bone's and its parent's (named in the pose's fourth word), last
    * frame's when asked; a root's parent the rest pose. A vertex at a child's pivot, its weight 0, sits where its parent
    * bone carries it: the joints stay closed.
    */
    void testVertexPoses() {
        const SwaySkeleton tree = threeLevelTree();
        const SwaySkeletonTables tables = packSwaySkeletons({&tree});
        const WindBoneMotion motion;
        const WindTreeShape shape{.height = tree.height, .footRadius = tree.bones[0].radius, .metresPerUnit = 1.0f, .seed = 9u};
        const std::vector<WindBonePose> now = boneTreePoses(tables, 0, shape, Vec2{0.2f, 0.1f}, 2.0f, motion);
        const std::vector<WindBonePose> last = boneTreePoses(tables, 0, shape, Vec2{-0.1f, 0.05f}, 1.98f, motion);
        const std::uint32_t firstPose = 6;
        // The slots before the tree's hold another tree's poses (not the rest pose), so a root reading past its tree shows.
        std::vector<GpuBonePose> pool(firstPose + 2 * now.size(), packBonePose(now.back(), 0));

        for (std::size_t bone = 0; bone < now.size(); ++bone) {
            pool[bonePoseSlot(firstPose, static_cast<std::uint32_t>(bone), false)] = packBonePose(now[bone], tree.bones[bone].parent);
            pool[bonePoseSlot(firstPose, static_cast<std::uint32_t>(bone), true)] = packBonePose(last[bone], tree.bones[bone].parent);
        }

        const auto same = [](const WindBonePose& a, const WindBonePose& b) {
            return near(a.turn.x, b.turn.x, 1e-6f) && near(a.turn.y, b.turn.y, 1e-6f) && near(a.turn.z, b.turn.z, 1e-6f) &&
                   near(a.turn.w, b.turn.w, 1e-6f) && near(a.shift.x, b.shift.x, 1e-6f) && near(a.shift.y, b.shift.y, 1e-6f) &&
                   near(a.shift.z, b.shift.z, 1e-6f);
        };

        const int child = 4;
        const auto parent = static_cast<std::size_t>(tree.bones[static_cast<std::size_t>(child)].parent);
        assert(tree.bones[static_cast<std::size_t>(child)].parent >= 0);
        const BoneVertexPoses bound = boneVertexPoses(pool, firstPose, packSwayBinding(SwayBinding{.bone = child, .weight = 0.25f}), false);
        assert(same(bound.own, now[static_cast<std::size_t>(child)]) && same(bound.parent, now[parent]) && near(bound.weight, 0.25f, 1e-4f));
        const BoneVertexPoses before = boneVertexPoses(pool, firstPose, packSwayBinding(SwayBinding{.bone = child, .weight = 0.25f}), true);
        assert(same(before.own, last[static_cast<std::size_t>(child)]) && same(before.parent, last[parent]));

        const BoneVertexPoses root = boneVertexPoses(pool, firstPose, packSwayBinding(SwayBinding{.bone = 0, .weight = 1.0f}), false);
        assert(same(root.own, now[0]) && same(root.parent, WindBonePose{}));

        const Vec3 pivot = tree.bones[static_cast<std::size_t>(child)].pivot;
        const BoneVertexPoses atJoint = boneVertexPoses(pool, firstPose, packSwayBinding(SwayBinding{.bone = child, .weight = 0.0f}), false);
        const Vec3 joint = windBlendedPlace(atJoint.own, atJoint.parent, atJoint.weight, pivot);
        assert((joint - windBonePlace(now[parent], pivot)).length() < 1e-5f && (joint - pivot).length() > 1e-3f);
    }

    // F4's BONES line while trees are posed: trees, the still ones, bones asked against the pool; none while none are.
    void testStatsLine() {
        RenderStats stats;
        const auto bonesLine = [](const std::vector<std::string>& lines) {
            return std::find_if(lines.begin(), lines.end(), [](const std::string& line) { return line.starts_with("BONES"); });
        };
        std::vector<std::string> lines = renderStatsLines(stats);
        assert(bonesLine(lines) == lines.end());
        stats.boneTrees = 120;
        stats.boneTreesStill = 3;
        stats.bonesAsked = 84000;
        stats.bonePool = 262144;
        lines = renderStatsLines(stats);
        const auto line = bonesLine(lines);
        assert(line != lines.end() && *line == "BONES TREES 120  STILL 3  BONES 84000/262144");
    }
}

void testBonePoses() {
    testChainsByLevel();
    testLevelPosesMatchListOrder();
    testScaleIsMetres();
    testLeanInPlant();
    testWantedPlants();
    testClaims();
    testPoolGrows();
    testLeanParts();
    testComparison();
    testTheTreesMove();
    testClaimedTreeAndSlots();
    testVertexPoses();
    testCompareCommand();
    testStatsLine();
}
