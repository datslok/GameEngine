#include "render/gpu/mesh_cluster_culling.h"
#include "render/gpu/instance_shared.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    // A model matrix's column (GpuInstance::model is column-major, as the shaders read it).
    Vec3 modelColumn(const GpuInstance& instance, int column) {
        return Vec3{instance.model[4 * column], instance.model[4 * column + 1], instance.model[4 * column + 2]};
    }

    // The next block's first pair at or after a count of pairs.
    std::uint32_t roundUpToBlock(std::uint32_t pairs) {
        return (pairs + meshClusterGroupSize - 1) / meshClusterGroupSize * meshClusterGroupSize;
    }

    // A group's pairs in its list: its drawn instances times its clusters.
    std::uint32_t groupPairs(const MeshClusterTables& tables, std::uint32_t group) {
        return tables.commands[group].instanceCount * tables.groups[group].clusterCount;
    }

    // Whether the pair is kept: its entry's draw's instance and record, and the cluster.
    bool pairKept(const MeshClusterTables& tables, const MeshClusterPair& pair, const GpuCameraCull& view) {
        const std::uint32_t entry = tables.drawList[pair.entry];
        const std::uint32_t draw = entry & DRAW_LIST_INSTANCE_MASK;
        return meshClusterKept(tables.clusters[pair.cluster], tables.instances[draw], entry, tables.records[draw], view);
    }
}

std::uint32_t meshClusterTriangleWords(std::uint32_t triangles) {
    return (triangles * 3 + 3) / 4;
}

/*
* Vertex numbers become the vertex pool's (the mesh's first vertex added), so a mesh shader needs no offset; each
* cluster's corner bytes start a word of their own, so a cluster reads whole words.
*/
PackedMeshClusters packMeshClusters(const MeshClusters& built, std::uint32_t meshFirstVertex, std::uint32_t firstClusterVertex,
                                    std::uint32_t firstTriangleWord) {
    PackedMeshClusters packed;
    packed.clusters.reserve(built.clusters.size());
    packed.vertices.reserve(built.vertices.size());

    for (const MeshCluster& cluster : built.clusters) {
        GpuMeshCluster record;
        record.sphere[0] = cluster.centre.x;
        record.sphere[1] = cluster.centre.y;
        record.sphere[2] = cluster.centre.z;
        record.sphere[3] = cluster.radius;
        record.cone[0] = cluster.coneApex.x;
        record.cone[1] = cluster.coneApex.y;
        record.cone[2] = cluster.coneApex.z;
        record.cone[3] = cluster.coneCutoff;
        record.axis[0] = cluster.coneAxis.x;
        record.axis[1] = cluster.coneAxis.y;
        record.axis[2] = cluster.coneAxis.z;
        record.firstVertex = firstClusterVertex + static_cast<std::uint32_t>(packed.vertices.size());
        record.firstTriangleWord = firstTriangleWord + static_cast<std::uint32_t>(packed.triangleWords.size());
        record.counts = cluster.vertexCount | (cluster.triangleCount << MESH_CLUSTER_TRIANGLE_SHIFT);
        record.meshFirstVertex = meshFirstVertex;
        packed.clusters.push_back(record);

        for (std::uint32_t vertex = 0; vertex < cluster.vertexCount; ++vertex) {
            packed.vertices.push_back(meshFirstVertex + built.vertices[cluster.firstVertex + vertex]);
        }

        const std::uint32_t words = meshClusterTriangleWords(cluster.triangleCount);
        const std::size_t start = packed.triangleWords.size();
        packed.triangleWords.resize(start + words, 0u);

        for (std::uint32_t corner = 0; corner < cluster.triangleCount * 3; ++corner) {
            const std::uint32_t value = built.triangles[cluster.firstTriangle * 3 + corner];
            packed.triangleWords[start + corner / 4] |= value << (8u * (corner % 4));
        }
    }

    return packed;
}

float meshClusterLargestScale(const GpuInstance& instance) {
    const Vec3 x = modelColumn(instance, 0);
    const Vec3 y = modelColumn(instance, 1);
    const Vec3 z = modelColumn(instance, 2);
    return std::sqrt(std::max(std::max(x.dot(x), y.dot(y)), z.dot(z)));
}

std::array<float, 4> meshClusterWorldSphere(const GpuMeshCluster& cluster, const GpuInstance& instance) {
    const Vec3 centre = modelColumn(instance, 0) * cluster.sphere[0] + modelColumn(instance, 1) * cluster.sphere[1] +
                        modelColumn(instance, 2) * cluster.sphere[2] + modelColumn(instance, 3);
    return {centre.x, centre.y, centre.z, cluster.sphere[3] * meshClusterLargestScale(instance)};
}

float meshClusterDeterminant(const GpuInstance& instance) {
    return modelColumn(instance, 0).dot(modelColumn(instance, 1).cross(modelColumn(instance, 2)));
}

// The inverse of the turning part is its columns' cross products (as rows) over its determinant.
Vec3 meshClusterEyeInMesh(const GpuInstance& instance, const Vec3& eye, float determinant) {
    const Vec3 x = modelColumn(instance, 0);
    const Vec3 y = modelColumn(instance, 1);
    const Vec3 z = modelColumn(instance, 2);
    const Vec3 offset = eye - modelColumn(instance, 3);
    return Vec3{y.cross(z).dot(offset), z.cross(x).dot(offset), x.cross(y).dot(offset)} / determinant;
}

bool meshClusterFacesAway(const GpuMeshCluster& cluster, const Vec3& eye) {
    MeshCluster cone;
    cone.coneApex = Vec3{cluster.cone[0], cluster.cone[1], cluster.cone[2]};
    cone.coneAxis = Vec3{cluster.axis[0], cluster.axis[1], cluster.axis[2]};
    cone.coneCutoff = cluster.cone[3];
    return clusterFacesAway(cone, eye);
}

bool meshClusterRigid(const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record, const GpuCameraCull& view) {
    if (instance.material[2] != 0u || instance.material[3] != INSTANCE_KIND_PLAIN || instance.hanging[3] != 0u) {
        return false;
    }

    if ((entry & DRAW_LIST_THINNED_BIT) != 0u) {
        return false;
    }

    const Vec3 eye{view.levels.eye[0], view.levels.eye[1], view.levels.eye[2]};
    return !((view.flags & CAMERA_CULL_SWAY) != 0u && (record.flags & CULL_SWAYS) != 0u && swayReaches(record.bounds, eye, view.swayFadeEnd));
}

bool meshClusterKept(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                     const GpuCameraCull& view) {
    if (!meshClusterRigid(instance, entry, record, view)) {
        return true;
    }

    const std::array<float, 4> sphere = meshClusterWorldSphere(cluster, instance);

    if (!sphereInFrustum(view.frustum, sphere.data())) {
        return false;
    }

    const float determinant = meshClusterDeterminant(instance);

    if ((record.flags & CULL_DOUBLE_SIDED) != 0u || !(determinant > 0.0f)) {
        return true;
    }

    const Vec3 eye{view.levels.eye[0], view.levels.eye[1], view.levels.eye[2]};
    return !meshClusterFacesAway(cluster, meshClusterEyeInMesh(instance, eye, determinant));
}

// The segments are the still ones, then the moving draws' (CameraCullFrame::join's order).
std::uint64_t meshClusterPairBound(const CameraCullFrame& table) {
    std::uint64_t bound = static_cast<std::uint64_t>(MESH_CLUSTER_STYLES) * meshClusterGroupSize;
    const std::size_t still = table.stillSegments.size();

    for (const GpuCullGroup& group : table.groups) {
        std::uint64_t candidates = 0;

        for (std::uint32_t segment = group.firstSegment; segment < group.firstSegment + group.segments; ++segment) {
            candidates += segment < still ? table.stillSegments[segment].candidates : table.movingSegments[segment - still].candidates;
        }

        bound += candidates * group.clusterCount;
    }

    return bound;
}

/*
* Style by style in the drawing order's runs, each from a block's first; a place's end is the pairs before it and its
* own. The runs must follow one another in style order, as cullDrawOrder lays them out.
*/
std::vector<std::uint32_t> meshClusterWorkEnds(const MeshClusterTables& tables, const GpuCameraCull& view,
                                               std::array<std::uint32_t, MESH_CLUSTER_WORK_WORDS>& work) {
    std::vector<std::uint32_t> ends;
    std::uint32_t carried = 0;
    std::uint32_t real = 0;
    work.fill(0);

    for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
        if (view.runFirst[style] != ends.size() && view.runCount[style] > 0) {
            throw std::invalid_argument("The drawing order's runs must follow one another in style order");
        }

        carried = roundUpToBlock(carried);
        work[MESH_CLUSTER_WORK_STYLE_FIRST + style] = carried;

        for (std::uint32_t place = view.runFirst[style]; place < view.runFirst[style] + view.runCount[style]; ++place) {
            const std::uint32_t pairs = groupPairs(tables, tables.drawOrder[place]);
            carried += pairs;
            real += pairs;
            ends.push_back(carried);
        }
    }

    work[MESH_CLUSTER_WORK_END] = roundUpToBlock(carried);
    work[MESH_CLUSTER_WORK_REAL] = real;
    return ends;
}

// The first place ending past the pair; none past the last place or in the padding before a style's first block.
MeshClusterPair meshClusterPairAt(const MeshClusterTables& tables, std::span<const std::uint32_t> ends, std::uint32_t pair) {
    const auto found = std::upper_bound(ends.begin(), ends.end(), pair);

    if (found == ends.end()) {
        return {};
    }

    const std::uint32_t place = static_cast<std::uint32_t>(found - ends.begin());
    const std::uint32_t group = tables.drawOrder[place];
    const std::uint32_t first = *found - groupPairs(tables, group);

    if (pair < first) {
        return {};
    }

    const std::uint32_t within = pair - first;
    const GpuCullGroup& culled = tables.groups[group];
    return MeshClusterPair{true, tables.commands[group].firstInstance + within / culled.clusterCount, culled.firstCluster + within % culled.clusterCount};
}

MeshClusterList cullMeshClustersOnCpu(const MeshClusterTables& tables, const GpuCameraCull& view, std::uint32_t room) {
    std::array<std::uint32_t, MESH_CLUSTER_WORK_WORDS> work{};
    const std::vector<std::uint32_t> ends = meshClusterWorkEnds(tables, view, work);
    const std::uint32_t blocks = work[MESH_CLUSTER_WORK_END] / meshClusterGroupSize;

    // Each block's survivors, then their prefix sum (one more for the total).
    std::vector<std::uint32_t> blockFirst(blocks + 1, 0);
    std::vector<MeshClusterPair> pairs(work[MESH_CLUSTER_WORK_END]);
    std::vector<bool> kept(pairs.size(), false);

    for (std::uint32_t index = 0; index < pairs.size(); ++index) {
        pairs[index] = meshClusterPairAt(tables, ends, index);
        kept[index] = pairs[index].real && pairKept(tables, pairs[index], view);
        blockFirst[index / meshClusterGroupSize + 1] += kept[index] ? 1u : 0u;
    }

    for (std::uint32_t block = 0; block < blocks; ++block) {
        blockFirst[block + 1] += blockFirst[block];
    }

    MeshClusterList list;
    list.tested = work[MESH_CLUSTER_WORK_REAL];
    list.kept = blockFirst[blocks];
    list.items.resize(std::min(list.kept, room));
    std::uint32_t within = 0;

    for (std::uint32_t index = 0; index < pairs.size(); ++index) {
        if (index % meshClusterGroupSize == 0) {
            within = 0;
        }

        if (kept[index]) {
            const std::uint32_t place = blockFirst[index / meshClusterGroupSize] + within;
            ++within;

            if (place < room) {
                list.items[place] = GpuMeshClusterItem{pairs[index].entry, pairs[index].cluster};
                list.triangles += tables.clusters[pairs[index].cluster].counts >> MESH_CLUSTER_TRIANGLE_SHIFT;
            }
        }
    }

    for (std::uint32_t style = 0; style < MESH_CLUSTER_STYLES; ++style) {
        const std::uint32_t firstBlock = work[MESH_CLUSTER_WORK_STYLE_FIRST + style] / meshClusterGroupSize;
        const std::uint32_t endBlock = (style + 1 < MESH_CLUSTER_STYLES ? work[MESH_CLUSTER_WORK_STYLE_FIRST + style + 1] : work[MESH_CLUSTER_WORK_END]) /
                                       meshClusterGroupSize;
        list.runFirst[style] = std::min(blockFirst[firstBlock], room);
        list.runCount[style] = std::min(blockFirst[endBlock], room) - list.runFirst[style];
    }

    return list;
}

bool nearAMeshClusterEdge(const GpuMeshCluster& cluster, const GpuInstance& instance, std::uint32_t entry, const GpuCullRecord& record,
                          const GpuCameraCull& view) {
    const Vec3 eye{view.levels.eye[0], view.levels.eye[1], view.levels.eye[2]};

    if (record.bounds[3] >= 0.0f) {
        const Vec3 centre{record.bounds[0], record.bounds[1], record.bounds[2]};
        const Vec3 offset = centre - eye;

        if (std::fabs(std::sqrt(offset.dot(offset)) - record.bounds[3] - view.swayFadeEnd) < 1e-3f) {
            return true;
        }
    }

    if (!meshClusterRigid(instance, entry, record, view)) {
        return false;
    }

    const std::array<float, 4> sphere = meshClusterWorldSphere(cluster, instance);

    for (const auto& plane : view.frustum.planes) {
        const float distance = plane[0] * sphere[0] + plane[1] * sphere[1] + plane[2] * sphere[2] + plane[3];

        if (std::fabs(distance + sphere[3]) < 1e-3f) {
            return true;
        }
    }

    const float determinant = meshClusterDeterminant(instance);

    if (cluster.cone[3] >= 1.0f || !(determinant > 0.0f)) {
        return false;
    }

    const Vec3 inMesh = meshClusterEyeInMesh(instance, eye, determinant);
    const Vec3 toApex = Vec3{cluster.cone[0], cluster.cone[1], cluster.cone[2]} - inMesh;
    const float distance = std::sqrt(toApex.dot(toApex));
    const float facing = toApex.dot(Vec3{cluster.axis[0], cluster.axis[1], cluster.axis[2]}) - cluster.cone[3] * distance;
    return std::fabs(facing) < 1e-4f * std::max(distance, 1.0f);
}

// Sorted, then walked together: equal pairs match, the rest are one side's only.
MeshClusterComparison compareMeshClusterLists(std::span<const GpuMeshClusterItem> gpu, std::span<const GpuMeshClusterItem> cpu,
                                              const std::function<bool(const GpuMeshClusterItem&)>& nearEdge) {
    const auto before = [](const GpuMeshClusterItem& a, const GpuMeshClusterItem& b) {
        return a.entry != b.entry ? a.entry < b.entry : a.cluster < b.cluster;
    };
    std::vector<GpuMeshClusterItem> gpuSorted(gpu.begin(), gpu.end());
    std::vector<GpuMeshClusterItem> cpuSorted(cpu.begin(), cpu.end());
    std::sort(gpuSorted.begin(), gpuSorted.end(), before);
    std::sort(cpuSorted.begin(), cpuSorted.end(), before);
    MeshClusterComparison comparison;
    comparison.entries = cpuSorted.size();
    std::size_t g = 0;
    std::size_t c = 0;

    while (g < gpuSorted.size() || c < cpuSorted.size()) {
        if (g < gpuSorted.size() && c < cpuSorted.size() && gpuSorted[g] == cpuSorted[c]) {
            ++comparison.matching;
            ++g;
            ++c;
        } else if (c < cpuSorted.size() && (g == gpuSorted.size() || before(cpuSorted[c], gpuSorted[g]))) {
            ++comparison.missing;
            comparison.nearAnEdge += nearEdge(cpuSorted[c]) ? 1u : 0u;
            ++c;
        } else {
            ++comparison.extra;
            comparison.nearAnEdge += nearEdge(gpuSorted[g]) ? 1u : 0u;
            ++g;
        }
    }

    return comparison;
}

std::string describeMeshClusterComparison(const MeshClusterComparison& comparison, std::uint32_t tested, std::uint32_t kept) {
    const bool agree = comparison.missing + comparison.extra == comparison.nearAnEdge;
    return "Mesh clusters: " + std::to_string(tested) + " tested, " + std::to_string(kept) + " kept; " + std::to_string(comparison.entries) +
           " on the CPU, " + std::to_string(comparison.matching) + " matching, " + std::to_string(comparison.missing) + " missing, " +
           std::to_string(comparison.extra) + " extra (" + std::to_string(comparison.nearAnEdge) + " near an edge). " +
           (agree ? "The GPU's list agrees with the CPU's." : "The GPU's list DIFFERS from the CPU's.");
}

std::array<std::uint32_t, 3> meshClusterTaskGroups(std::uint32_t count) {
    if (count == 0) {
        return {0, 1, 1};
    }

    return {std::min(count, MESH_CLUSTER_TASK_WIDTH), (count + MESH_CLUSTER_TASK_WIDTH - 1) / MESH_CLUSTER_TASK_WIDTH, 1};
}

std::uint32_t meshClusterTaskIndex(std::uint32_t x, std::uint32_t y) {
    return y * MESH_CLUSTER_TASK_WIDTH + x;
}

void packMeshClusterBuffers(const MeshClusterBuffers& buffers, std::uint32_t words[12]) {
    const std::uint64_t addresses[6] = {buffers.items, buffers.totals, buffers.clusters, buffers.clusterVertices, buffers.triangleWords, buffers.vertices};

    for (int buffer = 0; buffer < 6; ++buffer) {
        words[2 * buffer] = static_cast<std::uint32_t>(addresses[buffer] & 0xFFFFFFFFu);
        words[2 * buffer + 1] = static_cast<std::uint32_t>(addresses[buffer] >> 32u);
    }
}
