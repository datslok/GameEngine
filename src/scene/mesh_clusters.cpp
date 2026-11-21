#include "scene/mesh_clusters.h"

#include "meshoptimizer/meshoptimizer.h"

#define CLUSTERLOD_IMPLEMENTATION
#include "meshoptimizer/clusterlod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace {
    /*
    * How much a simplified group's error counts a turned normal against a moved surface: meshoptimizer's clusterlod
    * demo's weight, the light choice (the user's, phase 21 stage 2b), raised only if levels are seen to change shading.
    */
    constexpr float NORMAL_WEIGHT = 0.5f;

    // The attributes simplification reads, as floats from the normal on in MeshVertex: the normal (weighed), then the UV,
    // whose seams are kept (corners at one place with different UVs never merge, so the picture does not tear).
    constexpr std::size_t WEIGHED_ATTRIBUTES = 3;
    constexpr unsigned UV_SEAM_MASK = (1u << 3) | (1u << 4);

    // Turn one cluster clusterlod made into ours: its corners as a vertex run and local triangles, its sphere and cone.
    void addCluster(MeshClusters& built, const IndexedMesh& mesh, const clodCluster& made, std::uint32_t group) {
        std::array<std::uint32_t, MESH_CLUSTER_MAX_TRIANGLES * 3> vertices{};
        std::array<std::uint8_t, MESH_CLUSTER_MAX_TRIANGLES * 3> triangles{};
        const std::size_t vertexCount = clodLocalIndices(vertices.data(), triangles.data(), made.indices, made.index_count);
        const std::size_t triangleCount = made.index_count / 3;
        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(vertices.data(), triangles.data(), triangleCount,
                                                                   &mesh.vertices[0].position.x, mesh.vertices.size(), sizeof(MeshVertex));

        MeshCluster cluster;
        cluster.centre = Vec3{bounds.center[0], bounds.center[1], bounds.center[2]};
        cluster.radius = bounds.radius;
        cluster.coneApex = Vec3{bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]};
        cluster.coneAxis = Vec3{bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]};
        cluster.coneCutoff = bounds.cone_cutoff;
        cluster.firstVertex = static_cast<std::uint32_t>(built.vertices.size());
        cluster.vertexCount = static_cast<std::uint32_t>(vertexCount);
        cluster.firstTriangle = static_cast<std::uint32_t>(built.triangles.size() / 3);
        cluster.triangleCount = static_cast<std::uint32_t>(triangleCount);
        cluster.group = group;
        cluster.refinedGroup = made.refined < 0 ? meshClusterNoGroup : static_cast<std::uint32_t>(made.refined);
        built.clusters.push_back(cluster);

        built.vertices.insert(built.vertices.end(), vertices.begin(), vertices.begin() + vertexCount);
        built.triangles.insert(built.triangles.end(), triangles.begin(), triangles.begin() + made.index_count);
    }
}

MeshClusters buildMeshClusters(const IndexedMesh& mesh) {
    MeshClusters built;

    if (mesh.indices.empty()) {
        return built;
    }

    // Clusters as the mesh shaders take them; the rest clusterlod's defaults for rasterising (groups of about 16, half a
    // level, permissive simplification with a sloppy fallback, the error a parent's at least its children's).
    clodConfig config = clodDefaultConfig(MESH_CLUSTER_MAX_TRIANGLES);
    config.max_vertices = MESH_CLUSTER_MAX_VERTICES;

    const std::array<float, WEIGHED_ATTRIBUTES> weights{NORMAL_WEIGHT, NORMAL_WEIGHT, NORMAL_WEIGHT};
    clodMesh input{};
    input.indices = mesh.indices.data();
    input.index_count = mesh.indices.size();
    input.vertex_count = mesh.vertices.size();
    input.vertex_positions = &mesh.vertices[0].position.x;
    input.vertex_positions_stride = sizeof(MeshVertex);
    input.vertex_attributes = &mesh.vertices[0].normal.x;
    input.vertex_attributes_stride = sizeof(MeshVertex);
    input.attribute_weights = weights.data();
    input.attribute_count = WEIGHED_ATTRIBUTES;
    input.attribute_protect_mask = UV_SEAM_MASK;

    // Groups come level by level, so the mesh's own clusters (made from no group) come first.
    clodBuild(config, input, [&](clodGroup group, const clodCluster* clusters, std::size_t count) {
        const auto number = static_cast<std::uint32_t>(built.groups.size());
        const clodBounds& bounds = group.simplified;
        built.groups.push_back(MeshClusterGroup{Vec3{bounds.center[0], bounds.center[1], bounds.center[2]}, bounds.radius, bounds.error,
                                                static_cast<std::uint32_t>(group.depth)});

        for (std::size_t cluster = 0; cluster < count; ++cluster) {
            addCluster(built, mesh, clusters[cluster], number);

            if (clusters[cluster].refined < 0) {
                ++built.originalCount;
            }
        }

        return static_cast<int>(number);
    });

    return built;
}

std::vector<MeshClusters> buildMeshClusters(const std::vector<const IndexedMesh*>& meshes, JobSystem& jobs) {
    std::vector<std::size_t> largestFirst(meshes.size());

    for (std::size_t mesh = 0; mesh < meshes.size(); ++mesh) {
        largestFirst[mesh] = mesh;
    }

    // Ties keep the order asked, so the jobs are handed out the same way every run.
    std::stable_sort(largestFirst.begin(), largestFirst.end(), [&meshes](std::size_t first, std::size_t second) {
        return meshes[first]->indices.size() > meshes[second]->indices.size();
    });

    std::vector<MeshClusters> built(meshes.size());
    jobs.parallelFor(meshes.size(), 1, [&](std::size_t place) {
        const std::size_t mesh = largestFirst[place];
        built[mesh] = buildMeshClusters(*meshes[mesh]);
    });
    return built;
}

bool clusterFacesAway(const MeshCluster& cluster, const Vec3& eye) {
    if (cluster.coneCutoff >= 1.0f) {
        return false;
    }

    // The cosine between the axis and the direction from the eye to the apex, kept unnormalised: compared as
    // dot >= cutoff * length, with an eye on the apex (length 0) never culling.
    const Vec3 toApex = cluster.coneApex - eye;
    const float length = std::sqrt(toApex.dot(toApex));
    return length > 0.0f && toApex.dot(cluster.coneAxis) >= cluster.coneCutoff * length;
}
