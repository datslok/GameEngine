#include "scene/collision_mesh.h"

#include <stdexcept>

/*
* Move each corner once here, so collision queries against static geometry never transform anything per test.
* Indices are checked now because collision code reads them without checks, many times per tick.
*/
TriangleMesh collisionMesh(const Mesh& mesh, const Mat4& transform) {
    TriangleMesh result;
    result.positions.reserve(mesh.vertices.size());
    result.indices.reserve(mesh.triangles.size() * 3);

    for (const Vec4& vertex : mesh.vertices) {
        const Vec4 moved = transform * vertex;
        result.positions.push_back(Vec3{moved.x, moved.y, moved.z});
    }

    for (const Triangle& triangle : mesh.triangles) {
        for (const std::size_t index : {triangle.first, triangle.second, triangle.third}) {
            if (index >= mesh.vertices.size()) {
                throw std::invalid_argument("A triangle refers to a vertex the mesh does not have");
            }
            result.indices.push_back(static_cast<std::uint32_t>(index));
        }
    }

    return result;
}
