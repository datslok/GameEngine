#include "scene/mesh_clusters.h"

#include "meshoptimizer/meshoptimizer.h"

#include <cmath>
#include <cstddef>
#include <utility>

namespace {
    // How much the builder weighs keeping a cluster's facings alike against keeping it small: meshoptimizer's advice
    // when clusters are culled by their cones.
    constexpr float CONE_WEIGHT = 0.25f;
}

MeshClusters buildMeshClusters(const IndexedMesh& mesh) {
    MeshClusters built;

    if (mesh.indices.empty()) {
        return built;
    }

    // Room for the worst case, then cut down to what was made. Positions are the first three floats of each MeshVertex.
    const std::size_t most = meshopt_buildMeshletsBound(mesh.indices.size(), MESH_CLUSTER_MAX_VERTICES, MESH_CLUSTER_MAX_TRIANGLES);
    std::vector<meshopt_Meshlet> made(most);
    built.vertices.resize(most * MESH_CLUSTER_MAX_VERTICES);
    built.triangles.resize(most * MESH_CLUSTER_MAX_TRIANGLES * 3);
    const float* positions = &mesh.vertices[0].position.x;
    const std::size_t count = meshopt_buildMeshlets(made.data(), built.vertices.data(), built.triangles.data(), mesh.indices.data(),
                                                    mesh.indices.size(), positions, mesh.vertices.size(), sizeof(MeshVertex),
                                                    MESH_CLUSTER_MAX_VERTICES, MESH_CLUSTER_MAX_TRIANGLES, CONE_WEIGHT);
    made.resize(count);

    // The builder leaves gaps (each cluster's triangles start on a fresh row): pack the runs end to end.
    std::vector<std::uint32_t> packedVertices;
    std::vector<std::uint8_t> packedTriangles;
    built.clusters.reserve(count);

    for (const meshopt_Meshlet& piece : made) {
        std::uint32_t* vertices = &built.vertices[piece.vertex_offset];
        std::uint8_t* triangles = &built.triangles[piece.triangle_offset];
        meshopt_optimizeMeshlet(vertices, triangles, piece.triangle_count, piece.vertex_count);
        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(vertices, triangles, piece.triangle_count, positions,
                                                                   mesh.vertices.size(), sizeof(MeshVertex));

        MeshCluster cluster;
        cluster.centre = Vec3{bounds.center[0], bounds.center[1], bounds.center[2]};
        cluster.radius = bounds.radius;
        cluster.coneApex = Vec3{bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]};
        cluster.coneAxis = Vec3{bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]};
        cluster.coneCutoff = bounds.cone_cutoff;
        cluster.firstVertex = static_cast<std::uint32_t>(packedVertices.size());
        cluster.vertexCount = piece.vertex_count;
        cluster.firstTriangle = static_cast<std::uint32_t>(packedTriangles.size() / 3);
        cluster.triangleCount = piece.triangle_count;
        built.clusters.push_back(cluster);

        packedVertices.insert(packedVertices.end(), vertices, vertices + piece.vertex_count);
        packedTriangles.insert(packedTriangles.end(), triangles, triangles + piece.triangle_count * 3);
    }

    built.vertices = std::move(packedVertices);
    built.triangles = std::move(packedTriangles);
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
