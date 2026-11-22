#include "render/gpu/bone_records.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

std::uint32_t packSwayBinding(const SwayBinding& binding) {
    if (binding.bone < 0 || static_cast<std::size_t>(binding.bone) >= swayMostBones) {
        throw std::invalid_argument("A bound vertex's bone must be within a skeleton's 65535 bones");
    }

    const auto weight = static_cast<std::uint32_t>(std::lround(std::clamp(binding.weight, 0.0f, 1.0f) * 65535.0f));
    return static_cast<std::uint32_t>(binding.bone) | (weight << 16u);
}

SwayBinding unpackSwayBinding(std::uint32_t packed) {
    return SwayBinding{.bone = static_cast<int>(packed & 0xFFFFu), .weight = static_cast<float>(packed >> 16u) / 65535.0f};
}

SwaySkeletonTables packSwaySkeletons(const std::vector<const SwaySkeleton*>& skeletons) {
    SwaySkeletonTables tables;

    for (const SwaySkeleton* skeleton : skeletons) {
        if (skeleton->bones.empty() || skeleton->bones.size() > swayMostBones || !swayBonesOrdered(skeleton->bones)) {
            throw std::invalid_argument("A skeleton needs 1 to 65535 bones, parents first");
        }

        tables.skeletons.push_back(GpuSwaySkeleton{.firstBone = static_cast<std::uint32_t>(tables.bones.size()),
                                                   .boneCount = static_cast<std::uint32_t>(skeleton->bones.size()),
                                                   .height = skeleton->height,
                                                   .footRadius = skeleton->bones.front().radius});

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
