#pragma once

#include "scene/indexed_mesh.h"
#include "scene/mesh.h"
#include "scene/mesh_clusters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

// A sheet 1 m square of squares x squares (40 by default), rippled so there is shape to keep: as a scanned leaf or
// ground is, a curved surface with an open border.
inline IndexedMesh rippledSheet(int squares = 40) {
    IndexedMesh sheet = buildIndexedMesh(Mesh::grid(squares, squares, 1.0f, 1.0f));

    for (MeshVertex& vertex : sheet.vertices) {
        vertex.position.z = 0.05f * std::sin(6.0f * vertex.position.x) * std::cos(4.0f * vertex.position.y);
    }

    return sheet;
}

// The mesh vertex numbers merged where corners share a position (UV seams split corners, not the surface).
inline std::vector<std::uint32_t> positionNumbers(const IndexedMesh& mesh) {
    std::map<std::tuple<float, float, float>, std::uint32_t> first;
    std::vector<std::uint32_t> numbers(mesh.vertices.size());

    for (std::uint32_t vertex = 0; vertex < mesh.vertices.size(); ++vertex) {
        const Vec3& position = mesh.vertices[vertex].position;
        numbers[vertex] = first.emplace(std::make_tuple(position.x, position.y, position.z), vertex).first->second;
    }

    return numbers;
}

// A built cluster's triangle's corners as mesh vertex numbers.
inline std::array<std::uint32_t, 3> clusterTriangleCorners(const MeshClusters& built, const MeshCluster& cluster, std::uint32_t triangle) {
    std::array<std::uint32_t, 3> found{};

    for (std::uint32_t corner = 0; corner < 3; ++corner) {
        found[corner] = built.vertices[cluster.firstVertex + built.triangles[(cluster.firstTriangle + triangle) * 3 + corner]];
    }

    return found;
}

/*
* The edges of the drawn clusters' triangles left open: each edge (by position) counted forwards and backwards; a surface
* with no crack has every edge as often one way as the other, save along the mesh's own border. Triangles with two
* corners at one place (collapsed) have no area and are left out. numbers: positionNumbers. Returns the open edges as
* pairs of positions.
*/
inline std::vector<std::array<Vec3, 2>> openEdges(const IndexedMesh& mesh, const MeshClusters& built, const std::vector<std::uint32_t>& numbers,
                                                  const std::vector<std::uint32_t>& drawn) {
    std::vector<std::pair<std::uint64_t, int>> sides;

    for (std::uint32_t number : drawn) {
        const MeshCluster& cluster = built.clusters[number];

        for (std::uint32_t triangle = 0; triangle < cluster.triangleCount; ++triangle) {
            const std::array<std::uint32_t, 3> found = clusterTriangleCorners(built, cluster, triangle);
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

// The volume the drawn clusters' triangles enclose (each triangle's signed cone to the origin).
inline float enclosedVolume(const IndexedMesh& mesh, const MeshClusters& built, const std::vector<std::uint32_t>& drawn) {
    double volume = 0.0;

    for (std::uint32_t number : drawn) {
        const MeshCluster& cluster = built.clusters[number];

        for (std::uint32_t triangle = 0; triangle < cluster.triangleCount; ++triangle) {
            const std::array<std::uint32_t, 3> found = clusterTriangleCorners(built, cluster, triangle);
            const Vec3& a = mesh.vertices[found[0]].position;
            const Vec3& b = mesh.vertices[found[1]].position;
            const Vec3& c = mesh.vertices[found[2]].position;
            volume += a.dot(b.cross(c)) / 6.0;
        }
    }

    return static_cast<float>(volume);
}
