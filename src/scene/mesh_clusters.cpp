#include "scene/mesh_clusters.h"

#include "meshoptimizer/meshoptimizer.h"

#define CLUSTERLOD_IMPLEMENTATION
#include "meshoptimizer/clusterlod.h"

#include <algorithm>
#include <array>
#include <cfloat>
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

    // Clusters as the mesh shaders take them; the rest clusterlod's defaults for rasterising (groups of about 16, half a
    // level, permissive simplification with a sloppy fallback, the error a parent's at least its children's).
    clodConfig clusterConfig() {
        clodConfig config = clodDefaultConfig(MESH_CLUSTER_MAX_TRIANGLES);
        config.max_vertices = MESH_CLUSTER_MAX_VERTICES;
        return config;
    }

    /*
    * One cluster from its triangles as mesh vertex numbers, whichever path cut it: its corners as a vertex run and local
    * triangles (in order of first use), its sphere and cone.
    */
    void addCluster(MeshClusters& built, const IndexedMesh& mesh, const unsigned* indices, std::size_t indexCount, std::uint32_t group,
                    std::uint32_t refinedGroup) {
        std::array<std::uint32_t, MESH_CLUSTER_MAX_TRIANGLES * 3> vertices{};
        std::array<std::uint8_t, MESH_CLUSTER_MAX_TRIANGLES * 3> triangles{};
        const std::size_t vertexCount = clodLocalIndices(vertices.data(), triangles.data(), indices, indexCount);
        const std::size_t triangleCount = indexCount / 3;
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
        cluster.refinedGroup = refinedGroup;
        built.clusters.push_back(cluster);

        built.vertices.insert(built.vertices.end(), vertices.begin(), vertices.begin() + vertexCount);
        built.triangles.insert(built.triangles.end(), triangles.begin(), triangles.begin() + indexCount);
    }

    /*
    * The mesh's own clusters only, cut as clusterlod cuts its first level (its clusterize: the same builder, limits and
    * reordering), so they are the clusters the mesh would have with levels; all in one group, never simplified.
    */
    void buildOneLevel(MeshClusters& built, const IndexedMesh& mesh, const clodConfig& config) {
        const std::size_t most = meshopt_buildMeshletsBound(mesh.indices.size(), config.max_vertices, config.min_triangles);
        std::vector<meshopt_Meshlet> made(most);
        std::vector<unsigned> vertices(mesh.indices.size());
        std::vector<unsigned char> triangles(mesh.indices.size());
        const std::size_t count =
            meshopt_buildMeshletsFlex(made.data(), vertices.data(), triangles.data(), mesh.indices.data(), mesh.indices.size(), &mesh.vertices[0].position.x,
                                      mesh.vertices.size(), sizeof(MeshVertex), config.max_vertices, config.min_triangles, config.max_triangles, 0.0f,
                                      config.cluster_split_factor);
        std::vector<unsigned> indices;

        for (std::size_t piece = 0; piece < count; ++piece) {
            const meshopt_Meshlet& meshlet = made[piece];

            if (config.optimize_clusters) {
                meshopt_optimizeMeshletLevel(&vertices[meshlet.vertex_offset], meshlet.vertex_count, &triangles[meshlet.triangle_offset],
                                             meshlet.triangle_count, config.optimize_clusters_level);
            }

            indices.clear();

            for (std::size_t corner = 0; corner < meshlet.triangle_count * 3; ++corner) {
                indices.push_back(vertices[meshlet.vertex_offset + triangles[meshlet.triangle_offset + corner]]);
            }

            addCluster(built, mesh, indices.data(), indices.size(), 0, meshClusterNoGroup);
        }

        built.originalCount = static_cast<std::uint32_t>(built.clusters.size());
        const meshopt_Bounds bounds = meshopt_computeSphereBounds(&built.clusters[0].centre.x, built.clusters.size(), sizeof(MeshCluster),
                                                                  &built.clusters[0].radius, sizeof(MeshCluster));
        built.groups.push_back(MeshClusterGroup{Vec3{bounds.center[0], bounds.center[1], bounds.center[2]}, bounds.radius, FLT_MAX, 0});
    }

    // Every level (clusterlod), groups coming level by level, so the mesh's own clusters (made from no group) come first.
    void buildAllLevels(MeshClusters& built, const IndexedMesh& mesh, const clodConfig& config) {
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

        clodBuild(config, input, [&](clodGroup group, const clodCluster* clusters, std::size_t count) {
            const auto number = static_cast<std::uint32_t>(built.groups.size());
            const clodBounds& bounds = group.simplified;
            built.groups.push_back(MeshClusterGroup{Vec3{bounds.center[0], bounds.center[1], bounds.center[2]}, bounds.radius, bounds.error,
                                                    static_cast<std::uint32_t>(group.depth)});

            for (std::size_t cluster = 0; cluster < count; ++cluster) {
                const clodCluster& made = clusters[cluster];
                addCluster(built, mesh, made.indices, made.index_count, number,
                           made.refined < 0 ? meshClusterNoGroup : static_cast<std::uint32_t>(made.refined));

                if (made.refined < 0) {
                    ++built.originalCount;
                }
            }

            return static_cast<int>(number);
        });
    }
}

MeshClusters buildMeshClusters(const IndexedMesh& mesh, ClusterLevels levels) {
    MeshClusters built;

    if (mesh.indices.empty()) {
        return built;
    }

    if (levels == ClusterLevels::All) {
        buildAllLevels(built, mesh, clusterConfig());
    } else {
        buildOneLevel(built, mesh, clusterConfig());
    }

    return built;
}

std::vector<MeshClusters> buildMeshClusters(const std::vector<MeshClusterBuild>& meshes, JobSystem& jobs) {
    std::vector<std::size_t> largestFirst(meshes.size());

    for (std::size_t mesh = 0; mesh < meshes.size(); ++mesh) {
        largestFirst[mesh] = mesh;
    }

    // Ties keep the order asked, so the jobs are handed out the same way every run.
    std::stable_sort(largestFirst.begin(), largestFirst.end(), [&meshes](std::size_t first, std::size_t second) {
        return meshes[first].mesh->indices.size() > meshes[second].mesh->indices.size();
    });

    std::vector<MeshClusters> built(meshes.size());
    jobs.parallelFor(meshes.size(), 1, [&](std::size_t place) {
        const MeshClusterBuild& asked = meshes[largestFirst[place]];
        built[largestFirst[place]] = buildMeshClusters(*asked.mesh, asked.levels);
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
