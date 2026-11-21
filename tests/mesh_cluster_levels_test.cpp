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
#include <map>
#include <random>
#include <tuple>
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

    // The mesh vertex numbers merged where corners share a position (UV seams split corners, not the surface).
    std::vector<std::uint32_t> positionNumbers(const IndexedMesh& mesh) {
        std::map<std::tuple<float, float, float>, std::uint32_t> first;
        std::vector<std::uint32_t> numbers(mesh.vertices.size());

        for (std::uint32_t vertex = 0; vertex < mesh.vertices.size(); ++vertex) {
            const Vec3& position = mesh.vertices[vertex].position;
            numbers[vertex] = first.emplace(std::make_tuple(position.x, position.y, position.z), vertex).first->second;
        }

        return numbers;
    }

    // A drawn cluster's triangle's corners as mesh vertex numbers.
    std::array<std::uint32_t, 3> corners(const MeshClusters& built, const MeshCluster& cluster, std::uint32_t triangle) {
        std::array<std::uint32_t, 3> found{};

        for (std::uint32_t corner = 0; corner < 3; ++corner) {
            found[corner] = built.vertices[cluster.firstVertex + built.triangles[(cluster.firstTriangle + triangle) * 3 + corner]];
        }

        return found;
    }

    /*
    * The edges of the drawn triangles left open: each edge (by position) counted forwards and backwards; a surface with
    * no crack has every edge as often one way as the other, save along the mesh's own border. Triangles with two corners
    * at one place (collapsed) have no area and are left out. numbers: positionNumbers. Returns the open edges as pairs of
    * positions.
    */
    std::vector<std::array<Vec3, 2>> openEdges(const IndexedMesh& mesh, const MeshClusters& built, const std::vector<std::uint32_t>& numbers,
                                               const std::vector<std::uint32_t>& drawn) {
        std::vector<std::pair<std::uint64_t, int>> sides;

        for (std::uint32_t number : drawn) {
            const MeshCluster& cluster = built.clusters[number];

            for (std::uint32_t triangle = 0; triangle < cluster.triangleCount; ++triangle) {
                const std::array<std::uint32_t, 3> found = corners(built, cluster, triangle);
                const std::array<std::uint32_t, 3> merged{numbers[found[0]], numbers[found[1]], numbers[found[2]]};

                if (merged[0] == merged[1] || merged[1] == merged[2] || merged[2] == merged[0]) {
                    continue;
                }

                for (std::uint32_t edge = 0; edge < 3; ++edge) {
                    const std::uint32_t from = merged[edge];
                    const std::uint32_t to = merged[(edge + 1) % 3];

                    const std::uint64_t low = std::min(from, to);
                    const std::uint64_t high = std::max(from, to);
                    sides.emplace_back(low << 32 | high, from < to ? 1 : -1);
                }
            }
        }

        std::sort(sides.begin(), sides.end());
        std::vector<std::array<Vec3, 2>> open;

        for (std::size_t first = 0; first < sides.size();) {
            std::size_t last = first;
            int count = 0;

            for (; last < sides.size() && sides[last].first == sides[first].first; ++last) {
                count += sides[last].second;
            }

            if (count != 0) {
                const std::uint64_t edge = sides[first].first;
                open.push_back({mesh.vertices[edge >> 32].position, mesh.vertices[edge & 0xffffffffu].position});
            }

            first = last;
        }

        return open;
    }

    // The volume the drawn triangles enclose (each triangle's signed cone to the origin).
    float enclosedVolume(const IndexedMesh& mesh, const MeshClusters& built, const std::vector<std::uint32_t>& drawn) {
        double volume = 0.0;

        for (std::uint32_t number : drawn) {
            const MeshCluster& cluster = built.clusters[number];

            for (std::uint32_t triangle = 0; triangle < cluster.triangleCount; ++triangle) {
                const std::array<std::uint32_t, 3> found = corners(built, cluster, triangle);
                const Vec3& a = mesh.vertices[found[0]].position;
                const Vec3& b = mesh.vertices[found[1]].position;
                const Vec3& c = mesh.vertices[found[2]].position;
                volume += a.dot(b.cross(c)) / 6.0;
            }
        }

        return static_cast<float>(volume);
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
        const MeshClusters built = buildMeshClusters(ball);
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
        const MeshClusters built = buildMeshClusters(sheet);
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
        const MeshClusters one = buildMeshClusters(small);
        assert(one.clusters.size() == 1 && one.originalCount == 1 && one.groups.size() == 1);
        assert(one.groups[0].error == FLT_MAX);

        const IndexedMesh sheet = rippledSheet();
        assertSameClusters(buildMeshClusters(sheet), buildMeshClusters(sheet));
    }

    /*
    * Many meshes built at once on every thread (largest first) give each mesh what building it alone gives, in the
    * order asked: sheets of many sizes (an empty mesh among them), so the largest-first order differs from the order
    * asked.
    */
    void testManyBuiltAtOnce() {
        std::vector<IndexedMesh> meshes;

        for (int squares : {8, 60, 20, 100, 4, 40, 12, 80}) {
            meshes.push_back(rippledSheet(squares));
        }

        meshes.insert(meshes.begin() + 3, IndexedMesh{});
        std::vector<const IndexedMesh*> asked;

        for (const IndexedMesh& mesh : meshes) {
            asked.push_back(&mesh);
        }

        const std::vector<MeshClusters> together = buildMeshClusters(asked, testJobs());
        assert(together.size() == meshes.size());

        for (std::size_t mesh = 0; mesh < meshes.size(); ++mesh) {
            assertSameClusters(together[mesh], buildMeshClusters(meshes[mesh]));
        }

        assert(buildMeshClusters(std::vector<const IndexedMesh*>{}, testJobs()).empty());
    }
}

void testMeshClusterLevels() {
    testASpheresLevels();
    testASheetsLevels();
    testSmallAndRepeatable();
    testManyBuiltAtOnce();
}
