#include "scene/collision_mesh.h"

#include <stdexcept>

TriangleMesh collisionMesh(const Mesh& mesh, const Mat4& transform) {
    TriangleMesh result;
    appendCollisionMesh(result, mesh, transform);
    return result;
}

/*
* Move each corner once here, so collision queries against static geometry never transform anything per test. The new
* triangles' indices are shifted past the corners already there. Indices are checked now because collision code reads
* them without checks, many times per tick.
*/
void appendCollisionMesh(TriangleMesh& into, const Mesh& mesh, const Mat4& transform) {
    const std::size_t firstCorner = into.positions.size();
    into.positions.reserve(firstCorner + mesh.vertices.size());
    into.indices.reserve(into.indices.size() + mesh.triangles.size() * 3);

    for (const Triangle& triangle : mesh.triangles) {
        for (const std::size_t index : {triangle.first, triangle.second, triangle.third}) {
            if (index >= mesh.vertices.size()) {
                throw std::invalid_argument("A triangle refers to a vertex the mesh does not have");
            }
        }
    }

    for (const Vec4& vertex : mesh.vertices) {
        const Vec4 moved = transform * vertex;
        into.positions.push_back(Vec3{moved.x, moved.y, moved.z});
    }

    for (const Triangle& triangle : mesh.triangles) {
        for (const std::size_t index : {triangle.first, triangle.second, triangle.third}) {
            into.indices.push_back(static_cast<std::uint32_t>(firstCorner + index));
        }
    }
}
