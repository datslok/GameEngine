#include "scene/deforming_mesh.h"

#include "core/hash.h"
#include "scene/deforming_mesh_shared.h"

#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace {
    // What tells corners of one vertex apart: their UV (by exact bits) and their tangent's side.
    using CornerKey = std::tuple<std::uint32_t, std::uint32_t, bool>;

    // Shared with deform.glsl (scene/deforming_mesh_shared.h).
    constexpr float smallestDirection = static_cast<float>(DEFORM_SMALLEST_DIRECTION);
    constexpr float perpendicularSwitch = static_cast<float>(DEFORM_PERPENDICULAR_SWITCH);

    const Vec3 fallbackNormal{0.0f, 1.0f, 0.0f};

    // A unit vector square to the unit normal: across it from whichever axis it is furthest from lying along.
    Vec3 anyPerpendicular(const Vec3& normal) {
        const Vec3 axis = std::abs(normal.x) < perpendicularSwitch ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
        return normal.cross(axis).normalized();
    }

    std::vector<Vec3> sourcePositions(const Mesh& mesh) {
        std::vector<Vec3> positions;
        positions.reserve(mesh.vertices.size());
        for (const Vec4& vertex : mesh.vertices) {
            positions.push_back(Vec3{vertex.x, vertex.y, vertex.z});
        }
        return positions;
    }
}

void savePreviousShapes(World& world) {
    world.each<DeformingMesh>([](Entity, DeformingMesh& mesh) {
        mesh.previousPositions = mesh.positions;
    });
}

/*
* resolveCorners gives each triangle's corners their UV and tangent side (MikkTSpace's, which knows which way v runs);
* corners are then shared by their Mesh vertex, UV and side alone, since normals and tangents are worked out afresh.
*/
DeformingTopology buildDeformingTopology(const Mesh& mesh) {
    const std::vector<MeshVertex> corners = resolveCorners(mesh);

    DeformingTopology topology;
    topology.sourceVertexCount = mesh.vertices.size();
    std::map<std::pair<std::size_t, CornerKey>, std::uint32_t> sharedCorners;

    for (std::size_t triangle = 0; triangle < mesh.triangles.size(); ++triangle) {
        const Triangle& corners3 = mesh.triangles[triangle];
        const std::size_t sources[3]{corners3.first, corners3.second, corners3.third};

        for (std::size_t corner = 0; corner < 3; ++corner) {
            const MeshVertex& resolved = corners[triangle * 3 + corner];
            const CornerKey key{std::bit_cast<std::uint32_t>(resolved.uv.x), std::bit_cast<std::uint32_t>(resolved.uv.y), resolved.tangent.w < 0.0f};
            const auto [found, added] = sharedCorners.try_emplace({sources[corner], key}, static_cast<std::uint32_t>(topology.rest.vertices.size()));

            if (added) {
                MeshVertex vertex;
                vertex.uv = resolved.uv;
                vertex.tangent = Vec4{0.0f, 0.0f, 0.0f, resolved.tangent.w};
                topology.rest.vertices.push_back(vertex);
                topology.sourceVertex.push_back(static_cast<std::uint32_t>(sources[corner]));
            }
            topology.rest.indices.push_back(found->second);
        }
    }

    deformVertices(topology, sourcePositions(mesh), topology.rest.vertices);
    return topology;
}

Vec3 triangleAreaNormal(const Vec3& first, const Vec3& second, const Vec3& third) {
    return (second - first).cross(third - first);
}

// The solution of the triangle's edges as u and v change: edge1 dv2 - edge2 dv1, times the UV area's sign.
TriangleTangent triangleTangent(const Vec3& first, const Vec3& second, const Vec3& third, const Vec2& firstUv, const Vec2& secondUv,
                                const Vec2& thirdUv) {
    const Vec3 edge1 = second - first;
    const Vec3 edge2 = third - first;
    const Vec2 uvEdge1 = secondUv - firstUv;
    const Vec2 uvEdge2 = thirdUv - firstUv;
    const float uvArea = uvEdge1.x * uvEdge2.y - uvEdge2.x * uvEdge1.y;
    return TriangleTangent{(edge1 * uvEdge2.y - edge2 * uvEdge1.y) * (uvArea < 0.0f ? -1.0f : 1.0f), uvArea != 0.0f};
}

Vec3 deformedNormal(const Vec3& summed) {
    return summed.lengthSquared() > smallestDirection ? summed.normalized() : fallbackNormal;
}

Vec3 deformedTangent(const Vec3& summed, const Vec3& normal) {
    const Vec3 square = summed - normal * normal.dot(summed);
    return square.lengthSquared() > smallestDirection ? square.normalized() : anyPerpendicular(normal);
}

/*
* Each triangle's area-weighted normal goes to its corners' Mesh vertices (so corners split at a seam share it) and
* its tangent to its corners' render vertices (each side of a seam keeps its own).
*/
void deformVertices(const DeformingTopology& topology, std::span<const Vec3> positions, std::vector<MeshVertex>& vertices) {
    if (positions.size() != topology.sourceVertexCount) {
        throw std::invalid_argument("A deforming mesh needs one position per vertex of its mesh");
    }

    if (&vertices != &topology.rest.vertices) {
        vertices = topology.rest.vertices;
    }

    std::vector<Vec3> normals(topology.sourceVertexCount, Vec3{0.0f, 0.0f, 0.0f});
    std::vector<Vec3> tangents(vertices.size(), Vec3{0.0f, 0.0f, 0.0f});
    const std::vector<std::uint32_t>& indices = topology.rest.indices;

    for (std::size_t first = 0; first + 2 < indices.size(); first += 3) {
        const std::uint32_t corners[3]{indices[first], indices[first + 1], indices[first + 2]};
        const std::uint32_t sources[3]{topology.sourceVertex[corners[0]], topology.sourceVertex[corners[1]], topology.sourceVertex[corners[2]]};
        const Vec3 areaNormal = triangleAreaNormal(positions[sources[0]], positions[sources[1]], positions[sources[2]]);
        const TriangleTangent along = triangleTangent(positions[sources[0]], positions[sources[1]], positions[sources[2]], vertices[corners[0]].uv,
                                                      vertices[corners[1]].uv, vertices[corners[2]].uv);

        for (int corner = 0; corner < 3; ++corner) {
            normals[sources[corner]] = normals[sources[corner]] + areaNormal;
            if (along.hasUvArea) {
                tangents[corners[corner]] = tangents[corners[corner]] + along.alongU;
            }
        }
    }

    for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex) {
        const std::uint32_t source = topology.sourceVertex[vertex];
        const Vec3 normal = deformedNormal(normals[source]);
        const Vec3 tangent = deformedTangent(tangents[vertex], normal);

        MeshVertex& out = vertices[vertex];
        out.position = positions[source];
        out.normal = normal;
        out.tangent = Vec4{tangent.x, tangent.y, tangent.z, out.tangent.w};
    }
}

namespace {
    // Compressed rows from (owner, triangle) pairs already in triangle order: a counting sort keeps that order.
    void compressRows(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& pairs, std::size_t owners, std::vector<std::uint32_t>& starts,
                      std::vector<std::uint32_t>& triangles) {
        starts.assign(owners + 1, 0);
        for (const auto& [owner, triangle] : pairs) {
            ++starts[owner + 1];
        }
        for (std::size_t owner = 0; owner < owners; ++owner) {
            starts[owner + 1] += starts[owner];
        }
        triangles.assign(pairs.size(), 0);
        std::vector<std::uint32_t> next(starts.begin(), starts.end() - 1);
        for (const auto& [owner, triangle] : pairs) {
            triangles[next[owner]++] = triangle;
        }
    }
}

DeformingAdjacency buildDeformingAdjacency(const DeformingTopology& topology) {
    const std::vector<std::uint32_t>& indices = topology.rest.indices;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> bySource;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> byVertex;
    for (std::size_t first = 0; first + 2 < indices.size(); first += 3) {
        const std::uint32_t triangle = static_cast<std::uint32_t>(first / 3);
        for (std::size_t corner = 0; corner < 3; ++corner) {
            bySource.emplace_back(topology.sourceVertex[indices[first + corner]], triangle);
            byVertex.emplace_back(indices[first + corner], triangle);
        }
    }

    DeformingAdjacency adjacency;
    compressRows(bySource, topology.sourceVertexCount, adjacency.sourceStarts, adjacency.sourceTriangles);
    compressRows(byVertex, topology.rest.vertices.size(), adjacency.vertexStarts, adjacency.vertexTriangles);
    return adjacency;
}

// deformVertices' sums for one vertex, from zero in the same order, so the same floats come out.
MeshVertex deformVertexAt(const DeformingTopology& topology, const DeformingAdjacency& adjacency, std::span<const Vec3> positions,
                          std::uint32_t vertex) {
    const std::vector<std::uint32_t>& indices = topology.rest.indices;
    const std::vector<MeshVertex>& rest = topology.rest.vertices;
    const auto cornerPosition = [&](std::uint32_t triangle, std::uint32_t corner) {
        return positions[topology.sourceVertex[indices[triangle * 3 + corner]]];
    };

    Vec3 summedNormal{0.0f, 0.0f, 0.0f};
    const std::uint32_t source = topology.sourceVertex[vertex];
    for (std::uint32_t index = adjacency.sourceStarts[source]; index < adjacency.sourceStarts[source + 1]; ++index) {
        const std::uint32_t triangle = adjacency.sourceTriangles[index];
        summedNormal = summedNormal + triangleAreaNormal(cornerPosition(triangle, 0), cornerPosition(triangle, 1), cornerPosition(triangle, 2));
    }

    Vec3 summedTangent{0.0f, 0.0f, 0.0f};
    for (std::uint32_t index = adjacency.vertexStarts[vertex]; index < adjacency.vertexStarts[vertex + 1]; ++index) {
        const std::uint32_t triangle = adjacency.vertexTriangles[index];
        const TriangleTangent along = triangleTangent(cornerPosition(triangle, 0), cornerPosition(triangle, 1), cornerPosition(triangle, 2),
                                                      rest[indices[triangle * 3]].uv, rest[indices[triangle * 3 + 1]].uv,
                                                      rest[indices[triangle * 3 + 2]].uv);
        if (along.hasUvArea) {
            summedTangent = summedTangent + along.alongU;
        }
    }

    const Vec3 normal = deformedNormal(summedNormal);
    const Vec3 tangent = deformedTangent(summedTangent, normal);
    MeshVertex out = rest[vertex];
    out.position = positions[source];
    out.normal = normal;
    out.tangent = Vec4{tangent.x, tangent.y, tangent.z, out.tangent.w};
    return out;
}

std::vector<Vec3> blendedPositions(const DeformingMesh& mesh, float alpha) {
    if (mesh.previousPositions.size() != mesh.positions.size()) {
        return mesh.positions;
    }

    std::vector<Vec3> blended(mesh.positions.size());
    for (std::size_t vertex = 0; vertex < blended.size(); ++vertex) {
        blended[vertex] = mesh.previousPositions[vertex] + (mesh.positions[vertex] - mesh.previousPositions[vertex]) * alpha;
    }
    return blended;
}

DeformedMesh deformMesh(const DeformingTopology& topology, const DeformingMesh& mesh, float alpha) {
    const std::vector<Vec3> positions = blendedPositions(mesh, alpha);

    DeformedMesh deformed;
    deformed.mesh = mesh.mesh;
    deformVertices(topology, positions, deformed.vertices);

    Fnv1aHash hash;
    if (!positions.empty()) {
        deformed.bounds = Aabb{positions.front(), positions.front()};
    }
    for (const Vec3& position : positions) {
        hash.add(position.x);
        hash.add(position.y);
        hash.add(position.z);
        deformed.bounds = enclosing(deformed.bounds, Aabb{position, position});
    }
    deformed.shape = hash.value != 0 ? hash.value : 1;
    return deformed;
}
