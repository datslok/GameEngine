#include "render/gpu/bone_records.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace {
    /*
    * A skeleton's chains, level by level (in first bone order within each), and where each level starts among them
    * (one past the last for the end). Throws std::invalid_argument as packSwaySkeletons says.
    */
    void appendChainsByLevel(const std::vector<SwayBone>& bones, SwaySkeletonTables& tables) {
        std::map<int, std::vector<GpuSwayChain>> byLevel;

        for (std::size_t bone = 0; bone < bones.size();) {
            const SwayBone& first = bones[bone];

            if (first.chainStart != static_cast<int>(bone)) {
                throw std::invalid_argument("A chain must start at its chainStart");
            }

            if (first.parent >= 0 && bones[static_cast<std::size_t>(first.parent)].level >= first.level) {
                throw std::invalid_argument("A chain must start on a bone of a lower level");
            }

            std::size_t end = bone + 1;

            while (end < bones.size() && bones[end].chainStart == first.chainStart) {
                if (bones[end].parent != static_cast<int>(end - 1) || bones[end].level != first.level) {
                    throw std::invalid_argument("A chain's bones must each be the one before's child, of one level");
                }

                ++end;
            }

            byLevel[first.level].push_back(GpuSwayChain{static_cast<std::uint32_t>(bone), static_cast<std::uint32_t>(end - bone)});
            bone = end;
        }

        for (const auto& [level, chains] : byLevel) {
            tables.levelStarts.push_back(static_cast<std::uint32_t>(tables.chains.size()));
            tables.chains.insert(tables.chains.end(), chains.begin(), chains.end());
        }

        tables.levelStarts.push_back(static_cast<std::uint32_t>(tables.chains.size()));
    }
}

SwaySkeletonTables packSwaySkeletons(const std::vector<const SwaySkeleton*>& skeletons) {
    SwaySkeletonTables tables;

    for (const SwaySkeleton* skeleton : skeletons) {
        if (skeleton->bones.empty() || skeleton->bones.size() > swayMostBones || !swayBonesOrdered(skeleton->bones)) {
            throw std::invalid_argument("A skeleton needs 1 to 65534 bones, parents first");
        }

        const auto firstLevel = static_cast<std::uint32_t>(tables.levelStarts.size());
        appendChainsByLevel(skeleton->bones, tables);
        tables.skeletons.push_back(GpuSwaySkeleton{.firstBone = static_cast<std::uint32_t>(tables.bones.size()),
                                                   .boneCount = static_cast<std::uint32_t>(skeleton->bones.size()),
                                                   .height = skeleton->height,
                                                   .footRadius = skeleton->bones.front().radius,
                                                   .firstLevel = firstLevel,
                                                   .levelCount = static_cast<std::uint32_t>(tables.levelStarts.size()) - firstLevel - 1,
                                                   .reach = skeleton->reach});

        for (const SwayBone& bone : skeleton->bones) {
            tables.bones.push_back(GpuSwayBone{.pivot = {bone.pivot.x, bone.pivot.y, bone.pivot.z},
                                               .parent = bone.parent,
                                               .tip = {bone.tip.x, bone.tip.y, bone.tip.z},
                                               .radius = bone.radius,
                                               .chainStart = static_cast<std::uint32_t>(bone.chainStart),
                                               .chainLength = bone.chainLength,
                                               .chainRadius = bone.chainRadius,
                                               .level = static_cast<std::uint32_t>(bone.level)});
        }
    }

    return tables;
}

void placeSwayBindings(std::vector<std::uint32_t>& bindings, const IndexedMesh& mesh, const MeshPlacement& placement) {
    if (!mesh.swayBindings.empty() && (!placement.pooled || mesh.swayBindings.size() != mesh.vertices.size())) {
        throw std::invalid_argument("A bound mesh must be in the pools, with one binding a vertex");
    }

    if (!placement.pooled) {
        return;
    }

    const std::size_t end = std::size_t{placement.firstVertex} + placement.vertexCount;
    bindings.resize(std::max(bindings.size(), end), swayUnbound);

    for (std::size_t vertex = 0; vertex < mesh.swayBindings.size(); ++vertex) {
        bindings[placement.firstVertex + vertex] = packSwayBinding(mesh.swayBindings[vertex]);
    }
}
