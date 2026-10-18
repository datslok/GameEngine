#include "render/mesh_upload.h"

#include <limits>
#include <stdexcept>

void checkMeshUpload(const IndexedMesh& mesh) {
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
        throw std::invalid_argument("A mesh upload requires vertices and whole triangles of indices");
    }

    const std::size_t maxBytes = std::numeric_limits<std::uint32_t>::max();

    if (mesh.vertices.size() > maxBytes / sizeof(MeshVertex) ||
        mesh.indices.size() > maxBytes / sizeof(std::uint32_t) ||
        mesh.vertices.size() * sizeof(MeshVertex) > maxBytes - mesh.indices.size() * sizeof(std::uint32_t)) {
        throw std::overflow_error("Combined mesh upload is too large");
    }

    for (const std::uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            throw std::out_of_range("Mesh index references a missing vertex");
        }
    }
}

std::uint32_t vertexBytes(const IndexedMesh& mesh) {
    return static_cast<std::uint32_t>(mesh.vertices.size() * sizeof(MeshVertex));
}

std::uint32_t indexBytes(const IndexedMesh& mesh) {
    return static_cast<std::uint32_t>(mesh.indices.size() * sizeof(std::uint32_t));
}
