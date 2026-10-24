#include "scene/deforming_mesh.h"

#include "core/hash.h"

#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace {
    // What tells corners of one vertex apart: their UV (by exact bits) and their tangent's side.
    using CornerKey = std::tuple<std::uint32_t, std::uint32_t, bool>;

    // Below this a summed normal or tangent is taken as none: a shape squashed flat or to a point.
    constexpr float smallestDirection = 1e-12f;

    const Vec3 fallbackNormal{0.0f, 1.0f, 0.0f};

    // A unit vector square to the unit normal: across it from whichever axis it is furthest from lying along.
    Vec3 anyPerpendicular(const Vec3& normal) {
        const Vec3 axis = std::abs(normal.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
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

/*
* A triangle's cross product is its normal times twice its area, so summing them weights each triangle by its area.
* Its tangent is the solution of the triangle's edges as u and v change (Lengyel's), scaled by the sign of its UV area so
* a mirrored UV layout still points along increasing u.
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
        const Vec3 edge1 = positions[sources[1]] - positions[sources[0]];
        const Vec3 edge2 = positions[sources[2]] - positions[sources[0]];
        const Vec3 areaNormal = edge1.cross(edge2);

        const Vec2 uv0 = vertices[corners[0]].uv;
        const Vec2 uvEdge1 = vertices[corners[1]].uv - uv0;
        const Vec2 uvEdge2 = vertices[corners[2]].uv - uv0;
        const float uvArea = uvEdge1.x * uvEdge2.y - uvEdge2.x * uvEdge1.y;
        const Vec3 alongU = (edge1 * uvEdge2.y - edge2 * uvEdge1.y) * (uvArea < 0.0f ? -1.0f : 1.0f);

        for (int corner = 0; corner < 3; ++corner) {
            normals[sources[corner]] = normals[sources[corner]] + areaNormal;
            if (uvArea != 0.0f) {
                tangents[corners[corner]] = tangents[corners[corner]] + alongU;
            }
        }
    }

    for (std::size_t vertex = 0; vertex < vertices.size(); ++vertex) {
        const std::uint32_t source = topology.sourceVertex[vertex];
        const Vec3 summed = normals[source];
        const Vec3 normal = summed.lengthSquared() > smallestDirection ? summed.normalized() : fallbackNormal;
        const Vec3 square = tangents[vertex] - normal * normal.dot(tangents[vertex]);
        const Vec3 tangent = square.lengthSquared() > smallestDirection ? square.normalized() : anyPerpendicular(normal);

        MeshVertex& out = vertices[vertex];
        out.position = positions[source];
        out.normal = normal;
        out.tangent = Vec4{tangent.x, tangent.y, tangent.z, out.tangent.w};
    }
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
