#include "mesh_helpers.h"
#include "scene/mesh_clusters.h"
#include "scene/indexed_mesh.h"
#include "scene/mesh.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {
    // A closed round shape, finely cut as real meshes are (a cluster spans a few degrees): half its surface faces away
    // from any eye outside it.
    IndexedMesh sphere() {
        return buildIndexedMesh(Mesh::sphere(192, 96));
    }

    using Triangle = std::array<std::uint32_t, 3>;

    // A triangle turned so its smallest vertex comes first: the same triangle with the same winding compares equal.
    Triangle canonical(Triangle triangle) {
        while (triangle[0] > triangle[1] || triangle[0] > triangle[2]) {
            triangle = {triangle[1], triangle[2], triangle[0]};
        }

        return triangle;
    }

    // The mesh's triangles in mesh vertex numbers, cluster by cluster.
    std::vector<Triangle> clusterTriangles(const MeshClusters& built) {
        std::vector<Triangle> triangles;

        for (const MeshCluster& cluster : built.clusters) {
            for (std::uint32_t triangle = 0; triangle < cluster.triangleCount; ++triangle) {
                Triangle corners{};

                for (std::uint32_t corner = 0; corner < 3; ++corner) {
                    const std::uint8_t local = built.triangles[(cluster.firstTriangle + triangle) * 3 + corner];
                    corners[corner] = built.vertices[cluster.firstVertex + local];
                }

                triangles.push_back(corners);
            }
        }

        return triangles;
    }

    // Every cluster within the limits, its vertex and triangle runs inside the lists and its local corners inside its run.
    void assertWithinLimits(const IndexedMesh& mesh, const MeshClusters& built) {
        assert(built.triangles.size() % 3 == 0);

        for (const MeshCluster& cluster : built.clusters) {
            assert(cluster.vertexCount > 0 && cluster.vertexCount <= MESH_CLUSTER_MAX_VERTICES);
            assert(cluster.triangleCount > 0 && cluster.triangleCount <= MESH_CLUSTER_MAX_TRIANGLES);
            assert(cluster.firstVertex + cluster.vertexCount <= built.vertices.size());
            assert((cluster.firstTriangle + cluster.triangleCount) * 3 <= built.triangles.size());

            for (std::uint32_t vertex = 0; vertex < cluster.vertexCount; ++vertex) {
                assert(built.vertices[cluster.firstVertex + vertex] < mesh.vertices.size());
            }

            for (std::uint32_t corner = 0; corner < cluster.triangleCount * 3; ++corner) {
                assert(built.triangles[cluster.firstTriangle * 3 + corner] < cluster.vertexCount);
            }
        }
    }

    /*
    * Every triangle of the mesh in exactly one cluster, with its winding: the clusters' triangles, each turned to start at
    * its smallest vertex and sorted, are the mesh's.
    */
    void assertEveryTriangleOnce(const IndexedMesh& mesh, const MeshClusters& built) {
        std::vector<Triangle> expected;

        for (std::size_t first = 0; first < mesh.indices.size(); first += 3) {
            expected.push_back(canonical({mesh.indices[first], mesh.indices[first + 1], mesh.indices[first + 2]}));
        }

        std::vector<Triangle> found = clusterTriangles(built);

        for (Triangle& triangle : found) {
            triangle = canonical(triangle);
        }

        std::sort(expected.begin(), expected.end());
        std::sort(found.begin(), found.end());
        assert(found == expected);
    }

    // Each cluster's sphere holds every vertex of its own (a millionth of the radius spare for rounding).
    void assertSpheresHoldTheirVertices(const IndexedMesh& mesh, const MeshClusters& built) {
        for (const MeshCluster& cluster : built.clusters) {
            for (std::uint32_t vertex = 0; vertex < cluster.vertexCount; ++vertex) {
                const Vec3& position = mesh.vertices[built.vertices[cluster.firstVertex + vertex]].position;
                const Vec3 offset = position - cluster.centre;
                assert(std::sqrt(offset.dot(offset)) <= cluster.radius * 1.000001f + 1e-6f);
            }
        }
    }

    /*
    * From random eyes round and inside the mesh, a cluster said to face away has no triangle facing the eye: each
    * triangle's front (counterclockwise) side points away from the eye or is edge-on to it (within rounding). Returns the
    * share of the clusters said to face away over all the eyes, so callers can check the cone is of use.
    */
    float checkFacing(const IndexedMesh& mesh, const MeshClusters& built, float eyeDistance, std::uint32_t seed) {
        std::mt19937 random(seed);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        std::size_t culled = 0;
        std::size_t tested = 0;

        for (int eyeNumber = 0; eyeNumber < 200; ++eyeNumber) {
            const Vec3 eye{unit(random) * eyeDistance, unit(random) * eyeDistance, unit(random) * eyeDistance};

            for (const MeshCluster& cluster : built.clusters) {
                ++tested;

                if (!clusterFacesAway(cluster, eye)) {
                    continue;
                }

                ++culled;

                for (std::uint32_t triangle = 0; triangle < cluster.triangleCount; ++triangle) {
                    const std::uint32_t first = cluster.firstTriangle + triangle;
                    const Vec3& a = mesh.vertices[built.vertices[cluster.firstVertex + built.triangles[first * 3]]].position;
                    const Vec3& b = mesh.vertices[built.vertices[cluster.firstVertex + built.triangles[first * 3 + 1]]].position;
                    const Vec3& c = mesh.vertices[built.vertices[cluster.firstVertex + built.triangles[first * 3 + 2]]].position;
                    const Vec3 front = (b - a).cross(c - a);
                    const Vec3 toEye = eye - a;
                    const float frontLength = std::sqrt(front.dot(front));
                    const float eyeLength = std::sqrt(toEye.dot(toEye));
                    assert(front.dot(toEye) <= 1e-4f * frontLength * eyeLength);
                }
            }
        }

        return static_cast<float>(culled) / static_cast<float>(tested);
    }

    // A curved open sheet: every triangle once, within the limits, spheres holding, facing never wrong.
    void testASheetsClusters() {
        const IndexedMesh sheet = rippledSheet();
        const MeshClusters built = buildMeshClusters(sheet);
        assert(built.clusters.size() >= sheet.indices.size() / 3 / MESH_CLUSTER_MAX_TRIANGLES);
        assertWithinLimits(sheet, built);
        assertEveryTriangleOnce(sheet, built);
        assertSpheresHoldTheirVertices(sheet, built);
        checkFacing(sheet, built, 2.0f, 1u);
    }

    /*
    * A closed sphere: the same, and from eyes outside it the cone is of use: half its surface faces away, and the clusters
    * are grouped by facing, so over 40% of them are culled (47% measured).
    */
    void testASpheresClustersFaceAway() {
        const IndexedMesh ball = sphere();
        const MeshClusters built = buildMeshClusters(ball);
        assertWithinLimits(ball, built);
        assertEveryTriangleOnce(ball, built);
        assertSpheresHoldTheirVertices(ball, built);
        checkFacing(ball, built, 0.5f, 2u);
        const float culledFar = checkFacing(ball, built, 20.0f, 3u);
        assert(culledFar > 0.4f);
    }

    // No triangles, no clusters; building twice gives the same clusters (deterministic, as everything the GPU culls by).
    void testEmptyAndRepeatable() {
        const MeshClusters none = buildMeshClusters(IndexedMesh{});
        assert(none.clusters.empty() && none.vertices.empty() && none.triangles.empty());

        const IndexedMesh ball = sphere();
        const MeshClusters first = buildMeshClusters(ball);
        const MeshClusters second = buildMeshClusters(ball);
        assert(first.vertices == second.vertices && first.triangles == second.triangles);
        assert(first.clusters.size() == second.clusters.size());

        for (std::size_t cluster = 0; cluster < first.clusters.size(); ++cluster) {
            const MeshCluster& a = first.clusters[cluster];
            const MeshCluster& b = second.clusters[cluster];
            assert(a.firstVertex == b.firstVertex && a.vertexCount == b.vertexCount);
            assert(a.firstTriangle == b.firstTriangle && a.triangleCount == b.triangleCount);
            assert(a.radius == b.radius && a.coneCutoff == b.coneCutoff);
        }
    }
}

void testMeshClusters() {
    testASheetsClusters();
    testASpheresClustersFaceAway();
    testEmptyAndRepeatable();
}
