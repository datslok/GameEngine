#include "render/gpu/bone_poses.h"

#include "render/gpu/instance_shared.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace {
    // A part's skeleton number plus 1 (0: none), as the placed table keeps it.
    std::uint32_t partSkeleton(const GpuPlacedPart& part) {
        return part.unused[1];
    }

    float largestDifference(const float* a, const float* b, int count) {
        float worst = 0.0f;

        for (int i = 0; i < count; ++i) {
            worst = std::max(worst, std::abs(a[i] - b[i]));
        }

        return worst;
    }
}

GpuBonePose packBonePose(const WindBonePose& pose, std::int32_t parent) {
    return GpuBonePose{.turn = {pose.turn.x, pose.turn.y, pose.turn.z, pose.turn.w}, .shift = {pose.shift.x, pose.shift.y, pose.shift.z}, .parent = parent};
}

WindBonePose unpackBonePose(const GpuBonePose& pose) {
    return WindBonePose{.turn = Quat{pose.turn[0], pose.turn[1], pose.turn[2], pose.turn[3]}, .shift = Vec3{pose.shift[0], pose.shift[1], pose.shift[2]}};
}

std::vector<std::uint32_t> skeletonLeanParts(const PlacedTable& table, std::span<const DrawItem> plantedParts) {
    std::vector<std::uint32_t> leanParts(table.skeletons.size(), boneNoPart);

    for (std::size_t part = 0; part < table.parts.size(); ++part) {
        const std::uint32_t skeleton = partSkeleton(table.parts[part]);

        if (skeleton == 0 || leanParts.at(skeleton - 1) != boneNoPart) {
            continue;
        }

        if (plantedParts[table.draws.at(part)].sway.kind == SwayKind::Tree) {
            leanParts[skeleton - 1] = static_cast<std::uint32_t>(part);
        }
    }

    return leanParts;
}

std::vector<std::uint32_t> boneGroups(const PlacedTable& table) {
    std::vector<std::uint32_t> groups;

    for (const GpuPlacedPart& part : table.parts) {
        if (partSkeleton(part) != 0) {
            for (std::uint32_t slot = 0; slot <= part.levels.levels; ++slot) {
                groups.push_back(part.firstGroup + slot);
            }
        }
    }

    return groups;
}

// The plant's turn is Quat::fromAxisAngle(up, turn): x goes to (cos, -sin) in (x, z), z to (sin, cos); back by the transpose.
Vec2 windLeanInPlant(const Vec2& lean, float turnRadians) {
    const float c = std::cos(turnRadians);
    const float s = std::sin(turnRadians);
    return Vec2{lean.x * c - lean.y * s, lean.x * s + lean.y * c};
}

bool boneTreeWanted(const Vec3& foot, const Vec3& eye, float fadeEnd) {
    return (foot - eye).length() < fadeEnd;
}

std::uint32_t boneTreeFirstPose(std::uint32_t askedBefore, std::uint32_t boneCount, std::uint32_t poolBones) {
    return std::uint64_t{askedBefore} + boneCount <= poolBones ? 2u * askedBefore : boneNoPose;
}

std::uint32_t boneGrownPool(std::uint32_t pool, std::uint32_t bonesAsked, std::uint32_t most) {
    if (bonesAsked <= pool) {
        return pool;
    }

    // In 64 bits: the power of two above an ask past 2^31 does not fit 32.
    const std::uint64_t grown = std::min<std::uint64_t>(std::bit_ceil(std::uint64_t{bonesAsked}), most);
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(grown, pool));
}

std::uint32_t boneTreeSeed(const GpuPlacedPlant& plant) {
    return std::bit_cast<std::uint32_t>(plant.pick);
}

std::uint32_t boneNextStamp(std::uint32_t stamp) {
    return stamp == 0xFFFFFFFFu ? 1u : stamp + 1u;
}

std::uint32_t boneClaimedTree(std::uint32_t claimStamp, std::uint32_t claimTree, std::uint32_t frameStamp) {
    return frameStamp != 0 && claimStamp == frameStamp ? claimTree : boneNoTree;
}

std::uint32_t bonePoseSlot(std::uint32_t firstPose, std::uint32_t bone, bool previous) {
    return firstPose + 2u * bone + (previous ? 1u : 0u);
}

BoneVertexPoses boneVertexPoses(std::span<const GpuBonePose> pool, std::uint32_t firstPose, std::uint32_t binding, bool previous) {
    const SwayBinding bound = unpackSwayBinding(binding);
    const GpuBonePose& own = pool[bonePoseSlot(firstPose, static_cast<std::uint32_t>(bound.bone), previous)];
    const WindBonePose parent =
        own.parent < 0 ? WindBonePose{} : unpackBonePose(pool[bonePoseSlot(firstPose, static_cast<std::uint32_t>(own.parent), previous)]);
    return BoneVertexPoses{.own = unpackBonePose(own), .parent = parent, .weight = bound.weight};
}

std::vector<BoneWanted> boneWantedPlants(const PlacedTable& table, std::span<const GpuDrawCommand> commands, std::span<const GpuPlacedEntry> entries,
                                         std::span<const std::uint32_t> swayingViews, std::span<const GpuPlacedPlant> plants, const Vec3& eye,
                                         float fadeEnd) {
    const std::vector<std::uint32_t> groups = boneGroups(table);
    std::map<std::uint32_t, std::uint32_t> wanted;

    for (const std::uint32_t view : swayingViews) {
        for (const std::uint32_t group : groups) {
            const GpuDrawCommand& command = commands[std::size_t{view} * table.groups.size() + group];

            for (std::uint32_t instance = 0; instance < command.instanceCount; ++instance) {
                const GpuPlacedEntry& entry = entries[command.firstInstance - placedInstanceBase + instance];
                const std::uint32_t plant = entry.plant & DRAW_LIST_INSTANCE_MASK;
                const GpuPlacedPlant& placed = plants[plant];

                if (boneTreeWanted(Vec3{placed.foot[0], placed.foot[1], placed.foot[2]}, eye, fadeEnd)) {
                    wanted.emplace(plant, partSkeleton(table.parts[entry.part]) - 1);
                }
            }
        }
    }

    std::vector<BoneWanted> list;

    for (const auto& [plant, skeleton] : wanted) {
        list.push_back(BoneWanted{plant, skeleton});
    }

    return list;
}

BoneClaims claimBoneTrees(std::span<const BoneWanted> wanted, std::span<const GpuSwaySkeleton> skeletons, std::uint32_t poolBones) {
    BoneClaims claims;

    for (const BoneWanted& tree : wanted) {
        if (claims.trees.size() >= boneMostTrees) {
            break;
        }

        if (tree.skeleton >= skeletons.size()) {
            throw std::invalid_argument("A tree to pose names a skeleton past the table's");
        }

        const std::uint32_t bones = skeletons[tree.skeleton].boneCount;
        const std::uint32_t firstPose = boneTreeFirstPose(claims.bonesAsked, bones, poolBones);
        claims.trees.push_back(GpuBoneTree{.plant = tree.plant, .skeleton = tree.skeleton, .firstPose = firstPose});
        claims.bonesAsked += bones;
        claims.still += firstPose == boneNoPose ? 1u : 0u;
    }

    return claims;
}

std::vector<WindBonePose> boneTreePoses(const SwaySkeletonTables& tables, std::uint32_t skeleton, const WindTreeShape& tree, const Vec2& lean,
                                        float seconds, const WindBoneMotion& motion) {
    const GpuSwaySkeleton& record = tables.skeletons.at(skeleton);
    std::vector<WindBonePose> poses(record.boneCount);

    for (std::uint32_t level = 0; level < record.levelCount; ++level) {
        const std::uint32_t first = tables.levelStarts.at(record.firstLevel + level);
        const std::uint32_t end = tables.levelStarts.at(record.firstLevel + level + 1);

        for (std::uint32_t chain = first; chain < end; ++chain) {
            const GpuSwayChain& run = tables.chains.at(chain);

            for (std::uint32_t bone = run.firstBone; bone < run.firstBone + run.boneCount; ++bone) {
                const GpuSwayBone& b = tables.bones.at(record.firstBone + bone);
                const WindBoneShape shape{.pivot = Vec3{b.pivot[0], b.pivot[1], b.pivot[2]},
                                          .tip = Vec3{b.tip[0], b.tip[1], b.tip[2]},
                                          .radius = b.radius,
                                          .chainStart = b.chainStart,
                                          .chainLength = b.chainLength,
                                          .chainRadius = b.chainRadius};
                const WindBonePose parent = b.parent < 0 ? WindBonePose{} : poses[static_cast<std::size_t>(b.parent)];
                poses[bone] = windBonePose(parent, windBoneOwnTurn(shape, tree, lean, seconds, motion), shape.pivot);
            }
        }
    }

    return poses;
}

float boneTreeMove(const SwaySkeletonTables& tables, std::uint32_t skeleton, std::span<const WindBonePose> poses) {
    const GpuSwaySkeleton& record = tables.skeletons.at(skeleton);
    Vec2 largest{0.0f, 0.0f};

    for (std::uint32_t bone = 0; bone < record.boneCount; ++bone) {
        const GpuSwayBone& b = tables.bones.at(record.firstBone + bone);
        const Vec2 moves = windPoseMoves(poses[bone], Vec3{b.pivot[0], b.pivot[1], b.pivot[2]}, Vec3{b.tip[0], b.tip[1], b.tip[2]});
        largest = Vec2{std::max(largest.x, moves.x), std::max(largest.y, moves.y)};
    }

    return windTreeMove(largest, record.reach);
}

WindTreeShape boneTreeShape(const GpuSwaySkeleton& skeleton, const GpuPlacedPlant& plant) {
    return WindTreeShape{.height = skeleton.height, .footRadius = skeleton.footRadius, .metresPerUnit = plant.scale, .seed = boneTreeSeed(plant)};
}

BoneComparison compareBonePoses(std::span<const BoneWanted> wanted, std::span<const GpuBoneTree> trees, std::span<const GpuBonePose> poses,
                                const SwaySkeletonTables& tables, std::span<const GpuPlacedPlant> plants, const WindBoneMotion& motion, float seconds,
                                float previousSeconds) {
    BoneComparison comparison;
    comparison.wanted = wanted.size();
    comparison.claimed = trees.size();
    std::map<std::uint32_t, std::uint32_t> claimedTimes;

    for (const GpuBoneTree& tree : trees) {
        ++claimedTimes[tree.plant];
        comparison.still += tree.firstPose == boneNoPose ? 1u : 0u;
    }

    for (const auto& [plant, times] : claimedTimes) {
        comparison.twice += times > 1 ? 1u : 0u;
        // wanted is by plant (boneWantedPlants).
        const bool isWanted = std::binary_search(wanted.begin(), wanted.end(), BoneWanted{plant, 0},
                                                 [](const BoneWanted& a, const BoneWanted& b) { return a.plant < b.plant; });
        comparison.extra += isWanted ? 0u : 1u;
    }

    for (const BoneWanted& tree : wanted) {
        comparison.missing += claimedTimes.contains(tree.plant) ? 0u : 1u;
    }

    for (const GpuBoneTree& tree : trees) {
        if (tree.firstPose == boneNoPose || tree.skeleton >= tables.skeletons.size()) {
            continue;
        }

        const WindTreeShape shape = boneTreeShape(tables.skeletons[tree.skeleton], plants[tree.plant]);
        const std::vector<WindBonePose> now = boneTreePoses(tables, tree.skeleton, shape, Vec2{tree.lean[0], tree.lean[1]}, seconds, motion);
        const std::vector<WindBonePose> last =
            boneTreePoses(tables, tree.skeleton, shape, Vec2{tree.previousLean[0], tree.previousLean[1]}, previousSeconds, motion);
        comparison.worstMove = std::max(comparison.worstMove, std::abs(boneTreeMove(tables, tree.skeleton, now) - tree.move));

        for (std::size_t bone = 0; bone < now.size(); ++bone) {
            for (const auto& [expected, place] : {std::pair{&now[bone], 2 * bone}, std::pair{&last[bone], 2 * bone + 1}}) {
                const std::size_t at = tree.firstPose + place;

                if (at >= poses.size()) {
                    continue;
                }

                const GpuBonePose packed = packBonePose(*expected, tables.bones[tables.skeletons[tree.skeleton].firstBone + bone].parent);
                comparison.worstTurn = std::max(comparison.worstTurn, largestDifference(packed.turn, poses[at].turn, 4));
                comparison.worstShift = std::max(comparison.worstShift, largestDifference(packed.shift, poses[at].shift, 3));
                comparison.wrongParents += packed.parent == poses[at].parent ? 0u : 1u;
                ++comparison.posesChecked;
            }
        }
    }

    return comparison;
}

std::string describeBoneComparison(const BoneComparison& comparison) {
    const bool agrees = comparison.missing == 0 && comparison.extra == 0 && comparison.twice == 0 && comparison.wrongParents == 0 &&
                        comparison.worstTurn <= 1e-4f && comparison.worstShift <= 1e-3f &&
                        comparison.worstMove <= 1e-3f;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "Bones %s: %zu trees wanted, %zu claimed (%zu still for want of room), %zu missing, %zu extra, %zu twice; %zu poses checked, worst turn %.2g, "
                  "shift %.2g, move %.2g, %zu wrong parents.",
                  agrees ? "agree" : "DIFFER", comparison.wanted, comparison.claimed, comparison.still, comparison.missing, comparison.extra,
                  comparison.twice, comparison.posesChecked, static_cast<double>(comparison.worstTurn), static_cast<double>(comparison.worstShift),
                  static_cast<double>(comparison.worstMove), comparison.wrongParents);
    return line;
}
