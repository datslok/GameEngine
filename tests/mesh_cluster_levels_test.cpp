#include "mesh_helpers.h"
#include "test_jobs.h"
#include "scene/indexed_mesh.h"
#include "scene/mesh.h"
#include "scene/mesh_clusters.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace {
    // A closed round shape with UV seams (its corners split where the picture wraps), finely cut so it has many levels.
    IndexedMesh sphere() {
        return buildIndexedMesh(Mesh::sphere(192, 96));
    }

    // Whether a cut draws a cluster, given each group's error as the view sees it (larger: worse).
    using GroupError = std::function<float(std::uint32_t group)>;

    /*
    * The clusters a cut draws at a threshold: those whose group's error is above it (what was made from the group is too
    * coarse) and whose refined group's error is at or under it (what the cluster is good enough); the mesh's own
    * triangles count as error 0.
    */
    std::vector<std::uint32_t> cut(const MeshClusters& built, const GroupError& error, float threshold) {
        std::vector<std::uint32_t> drawn;

        for (std::uint32_t number = 0; number < built.clusters.size(); ++number) {
            const MeshCluster& cluster = built.clusters[number];
            const float own = cluster.refinedGroup == meshClusterNoGroup ? 0.0f : error(cluster.refinedGroup);

            if (error(cluster.group) > threshold && own <= threshold) {
                drawn.push_back(number);
            }
        }

        return drawn;
    }

    // Every group's own error, the same for any view.
    GroupError rawError(const MeshClusters& built) {
        return [&built](std::uint32_t group) { return built.groups[group].error; };
    }

    // Every group's error as seen from an eye: over its distance from the group's sphere (the near limit 0.01).
    GroupError seenError(const MeshClusters& built, const Vec3& eye) {
        return [&built, eye](std::uint32_t group) {
            const MeshClusterGroup& found = built.groups[group];
            const Vec3 offset = found.centre - eye;
            const float distance = std::sqrt(offset.dot(offset)) - found.radius;
            return found.error / std::max(distance, 0.01f);
        };
    }

    // Thresholds between and beyond every group's error, so each uniform cut is met once.
    std::vector<float> uniformThresholds(const MeshClusters& built) {
        std::vector<float> errors;

        for (const MeshClusterGroup& group : built.groups) {
            if (group.error < FLT_MAX) {
                errors.push_back(group.error);
            }
        }

        std::sort(errors.begin(), errors.end());
        errors.erase(std::unique(errors.begin(), errors.end()), errors.end());
        std::vector<float> thresholds{0.0f, FLT_MAX / 2.0f};

        for (std::size_t step = 0; step < 24 && !errors.empty(); ++step) {
            thresholds.push_back(errors[step * (errors.size() - 1) / 23]);
        }

        return thresholds;
    }

    /*
    * The hierarchy's shape: the mesh's own clusters first and only they made from no group, every group number in range,
    * a cluster's level one past the group it came from, coarser groups than the mesh's own, and all levels together
    * under 2.2 times the mesh's triangles (each level about half the last).
    */
    void assertShape(const IndexedMesh& mesh, const MeshClusters& built) {
        assert(built.originalCount > 0 && built.originalCount <= built.clusters.size());
        std::uint32_t triangles = 0;
        std::uint32_t deepest = 0;

        for (std::uint32_t number = 0; number < built.clusters.size(); ++number) {
            const MeshCluster& cluster = built.clusters[number];
            assert((number < built.originalCount) == (cluster.refinedGroup == meshClusterNoGroup));
            assert(cluster.group < built.groups.size());
            const std::uint32_t level = built.groups[cluster.group].level;

            if (cluster.refinedGroup == meshClusterNoGroup) {
                assert(level == 0);
            } else {
                assert(cluster.refinedGroup < built.groups.size());
                assert(level == built.groups[cluster.refinedGroup].level + 1);
            }

            triangles += cluster.triangleCount;
            deepest = std::max(deepest, level);
        }

        assert(deepest >= 3);
        assert(triangles < mesh.indices.size() / 3 * 22 / 10);
    }

    /*
    * A group bounds every group under it: each cluster's group has at least the error of the group it was made from, and
    * a sphere holding that group's sphere (a hundred-thousandth of the radius spare for rounding).
    */
    void assertParentsBoundChildren(const MeshClusters& built) {
        for (const MeshCluster& cluster : built.clusters) {
            if (cluster.refinedGroup == meshClusterNoGroup) {
                continue;
            }

            const MeshClusterGroup& parent = built.groups[cluster.group];
            const MeshClusterGroup& child = built.groups[cluster.refinedGroup];
            assert(parent.error >= child.error);
            const Vec3 offset = parent.centre - child.centre;
            assert(std::sqrt(offset.dot(offset)) + child.radius <= parent.radius * 1.00001f + 1e-6f);
        }
    }

    // The largest error among the drawn clusters (the mesh's own triangles 0): how far the cut's surface may stray.
    float largestError(const MeshClusters& built, const std::vector<std::uint32_t>& drawn) {
        float largest = 0.0f;

        for (std::uint32_t number : drawn) {
            const std::uint32_t refined = built.clusters[number].refinedGroup;
            largest = std::max(largest, refined == meshClusterNoGroup ? 0.0f : built.groups[refined].error);
        }

        return largest;
    }

    /*
    * The sphere: many levels, parents bounding children, and every cut, the same everywhere or seen from an eye, closed
    * (no open edge) and covering the sphere: a surface everywhere within its error of the sphere's encloses a volume
    * within the sphere's area times that error (the coarsest cut, 70 triangles with error 0.13, loses 18%, under the 37%
    * allowed; the finer cuts far less).
    */
    void testASpheresLevels() {
        const IndexedMesh ball = sphere();
        const MeshClusters built = buildMeshClusters(ball, ClusterLevels::All);
        assertShape(ball, built);
        assertParentsBoundChildren(built);

        std::vector<std::uint32_t> originals(built.originalCount);

        for (std::uint32_t number = 0; number < built.originalCount; ++number) {
            originals[number] = number;
        }

        const std::vector<std::uint32_t> numbers = positionNumbers(ball);
        assert(openEdges(ball, built, numbers, originals).empty());
        const float volume = enclosedVolume(ball, built, originals);
        const float area = 4.0f * 3.14159265f;
        assert(volume > 4.0f);
        const auto assertClosedAndFull = [&](const std::vector<std::uint32_t>& drawn) {
            assert(!drawn.empty());
            assert(openEdges(ball, built, numbers, drawn).empty());
            const float allowed = area * largestError(built, drawn) + 1e-4f * volume;
            assert(std::abs(enclosedVolume(ball, built, drawn) - volume) <= allowed);
        };

        for (float threshold : uniformThresholds(built)) {
            assertClosedAndFull(cut(built, rawError(built), threshold));
        }

        std::mt19937 random(7u);
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);

        for (int eyeNumber = 0; eyeNumber < 12; ++eyeNumber) {
            Vec3 direction{unit(random), unit(random), unit(random)};
            direction = direction * (1.0f / std::sqrt(direction.dot(direction)));
            const Vec3 eye = direction * (1.5f + 2.0f * static_cast<float>(eyeNumber));

            for (float threshold : {0.0005f, 0.002f, 0.01f}) {
                assertClosedAndFull(cut(built, seenError(built, eye), threshold));
            }
        }
    }

    // An open sheet: every cut's open edges lie on the sheet's own border (no crack inside), its area kept within 10%.
    void testASheetsLevels() {
        const IndexedMesh sheet = rippledSheet(160);
        const MeshClusters built = buildMeshClusters(sheet, ClusterLevels::All);
        assertShape(sheet, built);
        assertParentsBoundChildren(built);
        const std::vector<std::uint32_t> numbers = positionNumbers(sheet);
        const auto onBorder = [](const Vec3& position) {
            return std::abs(std::abs(position.x) - 0.5f) < 1e-5f || std::abs(std::abs(position.y) - 0.5f) < 1e-5f;
        };

        for (float threshold : uniformThresholds(built)) {
            const std::vector<std::uint32_t> drawn = cut(built, rawError(built), threshold);
            assert(!drawn.empty());

            for (const std::array<Vec3, 2>& edge : openEdges(sheet, built, numbers, drawn)) {
                assert(onBorder(edge[0]) && onBorder(edge[1]));
            }
        }
    }

    // Two builds the same: every list and record equal.
    void assertSameClusters(const MeshClusters& first, const MeshClusters& second) {
        assert(first.vertices == second.vertices && first.triangles == second.triangles);
        assert(first.originalCount == second.originalCount);
        assert(first.groups.size() == second.groups.size() && first.clusters.size() == second.clusters.size());

        for (std::size_t group = 0; group < first.groups.size(); ++group) {
            assert(first.groups[group].error == second.groups[group].error);
            assert(first.groups[group].radius == second.groups[group].radius);
            assert(first.groups[group].level == second.groups[group].level);
        }

        for (std::size_t cluster = 0; cluster < first.clusters.size(); ++cluster) {
            const MeshCluster& a = first.clusters[cluster];
            const MeshCluster& b = second.clusters[cluster];
            assert(a.firstVertex == b.firstVertex && a.firstTriangle == b.firstTriangle && a.triangleCount == b.triangleCount);
            assert(a.group == b.group && a.refinedGroup == b.refinedGroup && a.radius == b.radius);
        }
    }

    // One cluster's worth of triangles: one level, its group never simplified (drawn however far); building twice gives
    // the same hierarchy.
    void testSmallAndRepeatable() {
        const IndexedMesh small = buildIndexedMesh(Mesh::cube());
        const MeshClusters one = buildMeshClusters(small, ClusterLevels::All);
        assert(one.clusters.size() == 1 && one.originalCount == 1 && one.groups.size() == 1);
        assert(one.groups[0].error == FLT_MAX);

        const IndexedMesh sheet = rippledSheet();
        assertSameClusters(buildMeshClusters(sheet, ClusterLevels::All), buildMeshClusters(sheet, ClusterLevels::All));
    }

    // A cluster's triangles as mesh vertex numbers, in its own order.
    std::vector<std::uint32_t> clusterCorners(const MeshClusters& built, const MeshCluster& cluster) {
        std::vector<std::uint32_t> found;

        for (std::uint32_t triangle = 0; triangle < cluster.triangleCount; ++triangle) {
            const std::array<std::uint32_t, 3> three = clusterTriangleCorners(built, cluster, triangle);
            found.insert(found.end(), three.begin(), three.end());
        }

        return found;
    }

    /*
    * A mesh without levels (ClusterLevels::One): exactly the clusters a mesh with levels has at full detail (the same
    * triangles in the same clusters, in the same order within each; the clusters' order may differ, since levels list
    * them group by group), all in one group never simplified (drawn however far) whose sphere holds every cluster's;
    * cut, it is the mesh, closed.
    */
    void testOneLevel() {
        const IndexedMesh ball = sphere();
        const MeshClusters one = buildMeshClusters(ball, ClusterLevels::One);
        const MeshClusters all = buildMeshClusters(ball, ClusterLevels::All);
        assert(one.originalCount == one.clusters.size() && one.originalCount == all.originalCount);
        assert(one.groups.size() == 1 && one.groups[0].error == FLT_MAX && one.groups[0].level == 0);

        std::vector<std::vector<std::uint32_t>> fromOne;
        std::vector<std::vector<std::uint32_t>> fromAll;

        for (std::uint32_t number = 0; number < one.originalCount; ++number) {
            const MeshCluster& cluster = one.clusters[number];
            assert(cluster.group == 0 && cluster.refinedGroup == meshClusterNoGroup);
            const Vec3 offset = cluster.centre - one.groups[0].centre;
            assert(std::sqrt(offset.dot(offset)) + cluster.radius <= one.groups[0].radius * 1.00001f + 1e-6f);
            fromOne.push_back(clusterCorners(one, cluster));
            fromAll.push_back(clusterCorners(all, all.clusters[number]));
        }

        std::sort(fromOne.begin(), fromOne.end());
        std::sort(fromAll.begin(), fromAll.end());
        assert(fromOne == fromAll);

        const std::vector<std::uint32_t> drawn = cut(one, seenError(one, Vec3{0.0f, 0.0f, 50.0f}), 0.01f);
        assert(drawn.size() == one.clusters.size());
        assert(openEdges(ball, one, positionNumbers(ball), drawn).empty());
    }

    /*
    * Many meshes built at once on every thread (largest first) give each mesh what building it alone gives, in the
    * order asked, with or without levels as asked: sheets of many sizes (an empty mesh among them), so the largest-first
    * order differs from the order asked.
    */
    void testManyBuiltAtOnce() {
        std::vector<IndexedMesh> meshes;

        for (int squares : {8, 60, 20, 100, 4, 40, 12, 80}) {
            meshes.push_back(rippledSheet(squares));
        }

        meshes.insert(meshes.begin() + 3, IndexedMesh{});
        std::vector<MeshClusterBuild> asked;

        for (std::size_t mesh = 0; mesh < meshes.size(); ++mesh) {
            asked.push_back({&meshes[mesh], mesh % 3 == 1 ? ClusterLevels::One : ClusterLevels::All});
        }

        const std::vector<MeshClusters> together = buildMeshClusters(asked, testJobs());
        assert(together.size() == meshes.size());

        for (std::size_t mesh = 0; mesh < meshes.size(); ++mesh) {
            assertSameClusters(together[mesh], buildMeshClusters(meshes[mesh], asked[mesh].levels));
        }

        assert(together[1].groups.size() == 1 && together[2].groups.size() > 1);
        assert(buildMeshClusters(std::vector<MeshClusterBuild>{}, testJobs()).empty());
    }
}

void testMeshClusterLevels() {
    testASpheresLevels();
    testASheetsLevels();
    testSmallAndRepeatable();
    testOneLevel();
    testManyBuiltAtOnce();
}
