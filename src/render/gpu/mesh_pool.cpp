#include "render/gpu/mesh_pool.h"

#include <algorithm>
#include <stdexcept>

std::uint64_t poolCapacity(std::uint64_t current, std::uint64_t needed, std::uint64_t smallest, std::uint64_t elementBytes) {
    if (needed <= current) {
        return current;
    }

    std::uint64_t capacity = std::max(current, smallest);

    while (capacity < needed) {
        capacity *= 2;
    }

    // Doubling may overshoot the limit where the need itself does not: then exactly what is needed, or as much as fits.
    if (capacity * elementBytes > largestPoolBytes) {
        if (needed * elementBytes > largestPoolBytes) {
            throw std::length_error("The meshes do not fit the shared mesh buffer's 4 GB");
        }

        capacity = largestPoolBytes / elementBytes;
    }

    return capacity;
}

MeshPlacement MeshPoolLayout::place(std::uint32_t vertexCount, std::uint32_t indexCount) {
    const MeshPlacement placement{static_cast<std::uint32_t>(vertices), vertexCount, static_cast<std::uint32_t>(indices), indexCount, true};
    vertices += vertexCount;
    indices += indexCount;
    return placement;
}

ClusterPlacement MeshPoolLayout::placeClusters(std::uint32_t clusterCount, std::uint32_t vertexCount, std::uint32_t triangleWordCount) {
    const ClusterPlacement placement{static_cast<std::uint32_t>(clusters), static_cast<std::uint32_t>(clusterVertices),
                                     static_cast<std::uint32_t>(triangleWords)};
    clusters += clusterCount;
    clusterVertices += vertexCount;
    triangleWords += triangleWordCount;
    return placement;
}

std::uint64_t MeshPoolLayout::getClusters() const {
    return clusters;
}

std::uint64_t MeshPoolLayout::getClusterVertices() const {
    return clusterVertices;
}

std::uint64_t MeshPoolLayout::getTriangleWords() const {
    return triangleWords;
}

std::uint64_t MeshPoolLayout::getVertices() const {
    return vertices;
}

std::uint64_t MeshPoolLayout::getIndices() const {
    return indices;
}

GpuDrawCommand packDrawCommand(const DrawBatch& batch, const MeshPlacement& placement) {
    return GpuDrawCommand{placement.indexCount, batch.instanceCount, placement.firstIndex, static_cast<std::int32_t>(placement.firstVertex),
                          batch.firstInstance};
}

// A batch joins the run before it when both are pooled and drawn the same way.
std::vector<DrawRun> drawRuns(const std::vector<DrawBatch>& batches, const std::function<bool(MeshHandle)>& pooled) {
    std::vector<DrawRun> runs;

    for (std::size_t index = 0; index < batches.size(); ++index) {
        const DrawBatch& batch = batches[index];
        const bool inPool = pooled(batch.mesh);

        if (!runs.empty() && inPool && runs.back().pooled) {
            const DrawBatch& before = batches[index - 1];

            if (before.cutOut == batch.cutOut && before.doubleSided == batch.doubleSided) {
                ++runs.back().count;
                continue;
            }
        }

        runs.push_back(DrawRun{index, 1, inPool});
    }

    return runs;
}
