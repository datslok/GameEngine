#include "physics/raycast.h"
#include "scene/collision_mesh.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }
}

void testCollisionMesh() {
    // The cube (-1..1) moved 10 along x: every corner moves, every triangle keeps its three corners.
    const Mesh cube = Mesh::cube();
    const TriangleMesh moved = collisionMesh(cube, Mat4::translation(10.0f, 0.0f, 0.0f));

    assert(moved.positions.size() == cube.vertices.size());
    assert(moved.indices.size() == cube.triangles.size() * 3);
    assert(nearlyEqual(moved.positions[0].x, 9.0f));
    assert(nearlyEqual(moved.positions[6].x, 11.0f));
    assert(moved.indices[0] == cube.triangles[0].first);
    assert(moved.indices[1] == cube.triangles[0].second);
    assert(moved.indices[2] == cube.triangles[0].third);

    // A ray finds the moved cube where it now is.
    const std::optional<RayHit> hit = raycast(Ray{Vec3{10.0f, 0.0f, -5.0f}, Vec3{0.0f, 0.0f, 1.0f}}, moved);
    assert(hit.has_value());
    assert(nearlyEqual(hit->distance, 4.0f));
    assert(nearlyEqual(hit->normal.z, -1.0f));

    // Without a transform the mesh keeps its own coordinates.
    const TriangleMesh local = collisionMesh(cube);
    assert(nearlyEqual(local.positions[0].x, -1.0f));

    // A triangle that points at a missing corner is an error, not a crash later.
    Mesh broken = cube;
    broken.triangles[0].third = broken.vertices.size();
    bool threw = false;
    try {
        collisionMesh(broken);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
}
