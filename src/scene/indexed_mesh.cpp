#include "scene/indexed_mesh.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace {
    // The vertex's exact bits, so two corners only count as the same when every component is identical.
    using VertexKey = std::array<std::uint32_t, 8>;

    /*
    * Floats have two zeros: -0.0 == 0.0, but their bits differ, and cross products produce -0.0 easily (0 * -1).
    * Adding +0.0 turns -0.0 into +0.0 and leaves every other value unchanged, so equal corners get equal keys.
    */
    std::uint32_t bitsOf(float value) {
        return std::bit_cast<std::uint32_t>(value + 0.0f);
    }

    VertexKey makeKey(const MeshVertex& vertex) {
        return VertexKey{
            bitsOf(vertex.position.x),
            bitsOf(vertex.position.y),
            bitsOf(vertex.position.z),
            bitsOf(vertex.normal.x),
            bitsOf(vertex.normal.y),
            bitsOf(vertex.normal.z),
            bitsOf(vertex.uv.x),
            bitsOf(vertex.uv.y)
        };
    }

    // Mix each word into the running hash (the boost hash_combine recipe), so different vertices rarely collide.
    struct VertexKeyHash {
        std::size_t operator()(const VertexKey& key) const {
            std::size_t hash = 0;
            for (const std::uint32_t word : key) {
                hash ^= std::hash<std::uint32_t>{}(word) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };

    /*
    * The face normal from the cross product of two edges. Doubles keep long thin triangles accurate; a degenerate triangle keeps a zero normal.
    */
    Vec3 faceNormal(const Vec4& first, const Vec4& second, const Vec4& third) {
        const double edgeAX = static_cast<double>(second.x) - first.x;
        const double edgeAY = static_cast<double>(second.y) - first.y;
        const double edgeAZ = static_cast<double>(second.z) - first.z;

        const double edgeBX = static_cast<double>(third.x) - first.x;
        const double edgeBY = static_cast<double>(third.y) - first.y;
        const double edgeBZ = static_cast<double>(third.z) - first.z;

        const double normalX = edgeAY * edgeBZ - edgeAZ * edgeBY;
        const double normalY = edgeAZ * edgeBX - edgeAX * edgeBZ;
        const double normalZ = edgeAX * edgeBY - edgeAY * edgeBX;

        const double length = std::hypot(normalX, normalY, normalZ);

        if (length <= 0.0) {
            return Vec3{0.0f, 0.0f, 0.0f};
        }

        return Vec3{
            static_cast<float>(normalX / length),
            static_cast<float>(normalY / length),
            static_cast<float>(normalZ / length)
        };
    }
}

IndexedMesh buildIndexedMesh(const Mesh& mesh) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) {
        throw std::invalid_argument("An indexed mesh needs vertices and triangles");
    }

    // 32-bit indices: the corner count must fit.
    if (mesh.triangles.size() > std::numeric_limits<std::uint32_t>::max() / 3) {
        throw std::overflow_error("Mesh has too many triangles for 32-bit indices");
    }

    for (const Vec4& position : mesh.vertices) {
        if (!std::isfinite(position.x) ||
            !std::isfinite(position.y) ||
            !std::isfinite(position.z) ||
            position.w != 1.0f) {
            throw std::invalid_argument("Mesh positions must be finite XYZ with w = 1");
        }
    }

    IndexedMesh result;
    result.indices.reserve(mesh.triangles.size() * 3);

    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> indexOfVertex;

    for (const Triangle& triangle : mesh.triangles) {
        const std::array<std::size_t, 3> corners{triangle.first, triangle.second, triangle.third};

        for (const std::size_t corner : corners) {
            if (corner >= mesh.vertices.size()) {
                throw std::out_of_range("Mesh triangle references a missing vertex");
            }
        }

        const Vec3 face = faceNormal(
            mesh.vertices[corners[0]],
            mesh.vertices[corners[1]],
            mesh.vertices[corners[2]]
        );

        for (std::size_t corner = 0; corner < 3; ++corner) {
            const Vec4& position = mesh.vertices[corners[corner]];

            const MeshVertex vertex{
                Vec3{position.x, position.y, position.z},
                triangle.normals[corner].value_or(face),
                // Missing UVs sample the texture's corner; untextured parts use a white texture anyway.
                triangle.uvs[corner].value_or(Vec2{0.0f, 0.0f})
            };

            // Reuse an identical corner if there is one, otherwise append it.
            const auto [found, inserted] = indexOfVertex.try_emplace(
                makeKey(vertex),
                static_cast<std::uint32_t>(result.vertices.size())
            );

            if (inserted) {
                result.vertices.push_back(vertex);
            }

            result.indices.push_back(found->second);
        }
    }

    return result;
}
