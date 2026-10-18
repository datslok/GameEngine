#include "scene/indexed_mesh.h"
#include "scene/tangents.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace {
    // The vertex's exact bits, so two corners only count as the same when every component is identical.
    using VertexKey = std::array<std::uint32_t, 12>;

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
            bitsOf(vertex.uv.y),
            bitsOf(vertex.tangent.x),
            bitsOf(vertex.tangent.y),
            bitsOf(vertex.tangent.z),
            bitsOf(vertex.tangent.w)
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

    void checkMesh(const Mesh& mesh) {
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

        for (const Triangle& triangle : mesh.triangles) {
            for (const std::size_t corner : {triangle.first, triangle.second, triangle.third}) {
                if (corner >= mesh.vertices.size()) {
                    throw std::out_of_range("Mesh triangle references a missing vertex");
                }
            }
        }
    }

    // A file's tangents are used only when it gives one for every corner: a normal map baked against them needs all of them.
    bool hasEveryTangent(const Mesh& mesh) {
        return std::all_of(mesh.triangles.begin(), mesh.triangles.end(), [](const Triangle& triangle) {
            return std::all_of(triangle.tangents.begin(), triangle.tangents.end(), [](const auto& tangent) { return tangent.has_value(); });
        });
    }
}

std::vector<MeshVertex> resolveCorners(const Mesh& mesh) {
    checkMesh(mesh);

    std::vector<MeshVertex> corners;
    corners.reserve(mesh.triangles.size() * 3);
    const bool fileTangents = hasEveryTangent(mesh);

    for (const Triangle& triangle : mesh.triangles) {
        const std::array<std::size_t, 3> indices{triangle.first, triangle.second, triangle.third};

        const Vec3 face = faceNormal(
            mesh.vertices[indices[0]],
            mesh.vertices[indices[1]],
            mesh.vertices[indices[2]]
        );

        for (std::size_t corner = 0; corner < 3; ++corner) {
            const Vec4& position = mesh.vertices[indices[corner]];

            corners.push_back(MeshVertex{
                Vec3{position.x, position.y, position.z},
                triangle.normals[corner].value_or(face),
                // Missing UVs sample the texture's corner; untextured parts use a white texture anyway.
                triangle.uvs[corner].value_or(Vec2{0.0f, 0.0f}),
                fileTangents ? *triangle.tangents[corner] : Vec4{}
            });
        }
    }

    if (!fileTangents) {
        generateTangents(corners, mesh.uvOrigin);
    }

    return corners;
}

IndexedMesh buildIndexedMesh(const Mesh& mesh) {
    const std::vector<MeshVertex> corners = resolveCorners(mesh);

    IndexedMesh result;
    result.indices.reserve(corners.size());

    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> indexOfVertex;

    for (const MeshVertex& vertex : corners) {
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

    return result;
}
